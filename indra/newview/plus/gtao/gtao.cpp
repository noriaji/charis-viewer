/**
 * @file gtao.cpp
 * @brief Ground-truth ambient occlusion (GTAO) and contact shadows, written
 *        into the deferred light map. Registers with the plus foundation
 *        (research/92-gtao.md).
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

// Both off by default (PlusGtao, PlusContactShadows). Vulkan only: the passes
// are recorded by the backend (llvulkan/llvkgtao.cpp); with GL nothing is drawn.
//
// The official light map (mRT->deferredLight) holds the sun's shadow in r and
// ambient occlusion in g; softenLight (official, untouched) reads both. Each
// frame, after the sun program paints it and before the official blur:
//  1. GTAO, at 1/scale of the screen each way: the scene depth as view depth,
//     the occlusion (a few directions around each pixel, the highest horizon
//     on each side, the cosine-weighted visible arc between them), a 4x4
//     depth-aware average; then upsampled with the depth as the guide into g,
//     in place of the official SSAO's.
//  2. Contact shadows, at full resolution: a short ray from each pixel towards
//     the sun (or moon); where the depth buffer hides it, r is lowered. Each
//     2x2 quad averages its four rays. Close up a ray is kept to a share of
//     the screen, and they fade out when a face fills it (research/92 section 11).
// The official blur then softens both with the rest of the light map.
// When both run and the sun program left g at 1, GTAO's last pass (the
// upsample into g) and the contact shadows are one pass (PlusGtaoOnePass,
// research/125 #9): the same pixels, one read and write of the light map fewer.
//
// GTAO needs the official SSAO on (RenderDeferredSSAO): only then does
// softenLight read g, and only then does the blur run. Contact shadows need
// sun shadows (RenderShadowDetail 1 or more): only then does it read r.
//
// With screen space indirect light on (plus/ssgi, research/164) its passes
// take the place of the occlusion and the 4x4 average: a visibility bitmask
// over the same slices and samples gives the ambient occlusion GtaoApply
// writes into g, and with it the light the lighting adds. When SSGI cannot be
// drawn, GTAO's own passes draw as before.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "gtao.h"

#include "presets/presets.h"
#include "safemode/safemode.h"
#include "ssgi/ssgi.h"

#include "llappviewer.h"
#include "llenvironment.h"
#include "llfeaturemanager.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigtao.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "pipeline.h"

#include <cmath>
#include <string>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot (photo) is being drawn

namespace
{
    constexpr char SETTING_ENABLED[]        = "PlusGtao";
    constexpr char SETTING_QUALITY[]        = "PlusGtaoQuality";
    constexpr char SETTING_STRENGTH[]       = "PlusGtaoStrength";
    constexpr char SETTING_RADIUS[]         = "PlusGtaoRadius";
    constexpr char SETTING_CONTACT[]        = "PlusContactShadows";
    constexpr char SETTING_CONTACT_LENGTH[] = "PlusContactShadowLength";
    constexpr char SETTING_DEBUG_PASSES[]   = "PlusGtaoDebugPasses";
    constexpr char SETTING_DIAGNOSTICS[]    = "PlusGtaoDiagnostics";

    // PlusGtaoDebugPasses (research/92 section 10): how far the GTAO passes
    // go, to find which one a fault comes with. 0 every pass, as normal.
    enum DebugPasses : U32
    {
        DEBUG_ALL = 0,
        DEBUG_DEPTH_ONLY = 1,           // the view depth only
        DEBUG_TO_OCCLUSION = 2,         // and the occlusion
        DEBUG_TO_DENOISE = 3,           // and the 4x4 average; the light map is not written
        DEBUG_APPLY_NOTHING = 4,        // every pass, but the light map's g gets 1 (no occlusion)
        DEBUG_TARGETS_ONLY = 5,         // the targets are made and kept, no pass is recorded
    };

    // PlusGtaoQuality 1..3 (research/92, section 5); contact shadows follow it.
    struct QualityLevel
    {
        U32 scale;          // screen pixels per GTAO pixel, each way
        U32 slices;         // directions around each pixel
        U32 steps;          // samples on each side of a direction
        U32 contact_steps;  // samples along a contact shadow ray
    };
    constexpr QualityLevel QUALITY_LEVELS[] =
    {
        { 2, 1, 4,  8 },    // 1 low: a quarter of the pixels, 8 samples each
        { 2, 2, 6, 16 },    // 2 medium: a quarter of the pixels, 24 samples each
        { 1, 3, 4, 24 },    // 3 high: every pixel, 24 samples each
    };
    constexpr U32 FALLBACK_QUALITY = 2;

    constexpr F32 MIN_RADIUS = 0.1f;            // m (PlusGtaoRadius)
    constexpr F32 MAX_RADIUS = 5.f;
    constexpr F32 MAX_STRENGTH = 4.f;           // PlusGtaoStrength: the power on the visibility
    // The radius on screen at most, as a share of the screen's height: near
    // the camera a metre covers the screen, and samples that far apart only
    // miss the cache (the official SSAO caps it the same way).
    constexpr F32 MAX_SCREEN_RADIUS_SHARE = 0.2f;

    constexpr F32 MIN_CONTACT_LENGTH = 0.02f;   // m (PlusContactShadowLength)
    constexpr F32 MAX_CONTACT_LENGTH = 1.f;
    // How deep a surface in the depth buffer is taken to be: a ray passing
    // further behind it than this is not blocked (fingers, thin rails).
    constexpr F32 CONTACT_THICKNESS = 0.04f;
    // Contact shadows fade out between these view depths (m): beyond them a
    // ray is shorter than a pixel and the shadow maps do the work.
    constexpr F32 CONTACT_FADE_START = 25.f;
    constexpr F32 CONTACT_FADE_END = 40.f;
    // Close up (research/92 section 11) a ray covers at most this share of the
    // screen's height: its length in metres shrinks to fit, so its steps stay a
    // few pixels apart and their start offsets do not show as dots. About 115
    // pixels at 1440p: from a 60-degree camera a 0.3 m ray is kept whole from
    // about 3.3 m away.
    constexpr F32 CONTACT_MAX_SCREEN_SHARE = 0.08f;
    // And contact shadows fade out where a metre spans this many screen heights
    // at the pixel's depth: from about 0.87 m (1) to 0.43 m (2) from a 60-degree
    // camera. A face filling the screen is left to the shadow maps, fitted to
    // the head there (plus/shadowdist).
    constexpr F32 CONTACT_NEAR_FADE_START = 1.f;
    constexpr F32 CONTACT_NEAR_FADE_END = 2.f;

    // Diagnostics (PlusGtaoDiagnostics, off): a line every few seconds.
    constexpr F32 DIAGNOSTICS_INTERVAL = 3.f;   // s

    // replacesAmbientOcclusion() told the pipeline to draw the sun program
    // without the official SSAO this frame; render() reads and clears it.
    bool sSunWithoutSsao = false;
    // A frame left the official SSAO out and then GTAO could not be drawn
    // (that frame had no ambient occlusion): the sun program keeps the
    // official SSAO until frame sRetryFrame, then leaving it out is tried
    // again. Each failure in a row doubles the wait; a frame drawn resets it.
    constexpr U32 RETRY_FIRST_WAIT = 300;                   // frames
    constexpr U32 RETRY_MAX_WAIT = RETRY_FIRST_WAIT * 64;   // about 5 minutes at 60 fps
    bool sWaitingToRetry = false;
    U32 sRetryFrame = 0;
    U32 sRetryWait = RETRY_FIRST_WAIT;

    bool keepingOfficialSsao()
    {
        if (sWaitingToRetry && (S32)(LLFrameTimer::getFrameCount() - sRetryFrame) >= 0)
        {
            sWaitingToRetry = false;
        }
        return sWaitingToRetry;
    }

    struct Settings
    {
        bool ao = false;
        bool contact = false;
        QualityLevel level = QUALITY_LEVELS[FALLBACK_QUALITY - 1];
        F32 radius = 0.f;           // m
        F32 power = 1.f;
        F32 contact_length = 0.f;   // m
        bool gi = false;            // the occlusion is plus/ssgi's (with ao only)
    };

    U32 qualityIndex(U32 quality)
    {
        const U32 level_count = (U32)(sizeof(QUALITY_LEVELS) / sizeof(QUALITY_LEVELS[0]));
        return llclamp(quality, 1u, level_count) - 1;
    }

    // For a light map of width x height.
    Settings readSettings(U32 width, U32 height)
    {
        static LLCachedControl<bool> ao(gSavedSettings, SETTING_ENABLED, false);
        static LLCachedControl<U32>  quality(gSavedSettings, SETTING_QUALITY, FALLBACK_QUALITY);
        static LLCachedControl<F32>  strength(gSavedSettings, SETTING_STRENGTH, 1.f);
        static LLCachedControl<F32>  radius(gSavedSettings, SETTING_RADIUS, 0.75f);
        static LLCachedControl<bool> contact(gSavedSettings, SETTING_CONTACT, false);
        static LLCachedControl<F32>  contact_length(gSavedSettings, SETTING_CONTACT_LENGTH, 0.3f);
        // A snapshot with the boost on (plus/presets): both on, at the best quality
        // its size allows (never below the user's own while they have them on).
        const bool boost = plus::presets::snapshotBoostActive();
        U32 wanted = quality;
        if (boost)
        {
            const U32 boosted = plus::presets::snapshotBoostQuality(width, height);
            wanted = (ao || contact) ? llmax(boosted, wanted) : boosted;
        }
        Settings out;
        out.level = QUALITY_LEVELS[qualityIndex(wanted)];
        out.power = llclamp((F32)strength, 0.f, MAX_STRENGTH);
        out.ao = (ao || boost) && out.power > 0.f;
        out.radius = llclamp((F32)radius, MIN_RADIUS, MAX_RADIUS);
        out.contact = contact || boost;
        out.contact_length = llclamp((F32)contact_length, MIN_CONTACT_LENGTH, MAX_CONTACT_LENGTH);
        out.gi = out.ao && plus::ssgi::wanted();
        return out;
    }

    U32 debugPasses()
    {
        static LLCachedControl<U32> debug_passes(gSavedSettings, SETTING_DEBUG_PASSES, DEBUG_ALL);
        return debug_passes;
    }

    // ------------------------------------------------------------------
    // The GTAO targets, at 1/scale of the screen. Made on first use and kept
    // while GTAO is on; never destroyed by a static destructor, which would
    // run without a GL context.

    struct Targets
    {
        LLRenderTarget view_depth;  // R32F: view depth (m)
        LLRenderTarget occlusion;   // RG16F: visibility, view depth
        LLRenderTarget denoised;    // RG16F: the same, averaged
        plus::RetryWait retry;      // after one could not be made (VRAM full)
    };

    Targets& targets()
    {
        static Targets* targets = new Targets();
        return *targets;
    }

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
    }

    void releaseTargets()
    {
        Targets& t = targets();
        releaseTarget(t.view_depth);
        releaseTarget(t.occlusion);
        releaseTarget(t.denoised);
    }

    bool sizeTarget(LLRenderTarget& target, U32 width, U32 height, U32 format)
    {
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        plus::RetryWait& retry = targets().retry;
        if (!retry.due())
        {
            return false;       // one could not be made a moment ago: not again every frame
        }
        if (!target.allocate(width, height, format))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            retry.failed();
            LL_WARNS_ONCE("PlusGtao") << "GTAO: no target of " << width << "x" << height << LL_ENDL;
            return false;
        }
        retry.succeeded();
        // The passes read texels as they are (texelFetch): point sampling, which
        // every float format supports (R32F need not filter linearly). Then unit 0
        // is left empty, not holding the new texture in the middle of the lighting.
        target.bindTexture(0, 0, LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // Whose passes give the occlusion this frame.
    enum class Occlusion
    {
        None,       // a target could not be made: no GTAO this frame
        Gtao,       // GTAO's occlusion and 4x4 average, into its own targets
        Ssgi,       // plus/ssgi's, into its targets (GTAO's two are released)
    };

    // The view depth and the occlusion's targets at width x height: SSGI's
    // when gi (its settings are on) and they can be made, else GTAO's own.
    Occlusion sizeTargets(U32 width, U32 height, bool gi)
    {
        Targets& t = targets();
        if (!sizeTarget(t.view_depth, width, height, GL_R32F))
        {
            return Occlusion::None;
        }
        if (gi && plus::ssgi::sizeTargets(width, height))
        {
            releaseTarget(t.occlusion);
            releaseTarget(t.denoised);
            return Occlusion::Ssgi;
        }
        return sizeTarget(t.occlusion, width, height, GL_RG16F) && sizeTarget(t.denoised, width, height, GL_RG16F) ? Occlusion::Gtao
                                                                                                                  : Occlusion::None;
    }

    // ------------------------------------------------------------------

    bool backendReady()
    {
        return LLRHI::sGtao && LLRHI::sGtaoDepth && LLRHI::sGtaoOcclusion && LLRHI::sGtaoDenoise && LLRHI::sGtaoApply &&
               LLRHI::sContactShadows;
    }

    // The main view's light map, painted and not bound: not a reflection
    // probe face or a mirror (they have light maps of their own).
    bool mainLightMapReady()
    {
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        return rt.deferredLight.isComplete() && !rt.deferredLight.isBoundInStack() && rt.deferredScreen.getDepth() &&
               rt.deferredLight.getWidth() == rt.deferredScreen.getWidth() &&
               rt.deferredLight.getHeight() == rt.deferredScreen.getHeight();
    }

    LLRHI::GtaoView viewOf(const glm::mat4& projection)
    {
        const LLRenderTarget& screen = gPipeline.mRT->deferredScreen;
        LLRHI::GtaoView view;
        view.depth_a = projection[2][2];
        view.depth_b = projection[3][2];
        view.projection[0] = projection[0][0];
        view.projection[1] = projection[1][1];
        view.projection[2] = projection[2][0];
        view.projection[3] = projection[2][1];
        view.depth_texture = screen.getDepth();
        view.width = screen.getWidth();
        view.height = screen.getHeight();
        return view;
    }

    // The direction (eye space) of the light that cast this frame's sun
    // shadows (LLPipeline::generateSunShadow: the sun while it is up, else
    // the moon); false when there is none.
    bool shadowingLightDirection(F32 out[3])
    {
        const bool sun = LLEnvironment::instance().getIsSunUp();
        const LLVector4& towards = sun ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir;
        LLVector3 direction(towards.mV[VX], towards.mV[VY], towards.mV[VZ]);
        if (direction.normalize() <= 0.f)
        {
            return false;
        }
        for (U32 c = 0; c < 3; ++c)
        {
            out[c] = direction.mV[c];
        }
        return true;
    }

    // Full-resolution pixels a contact shadow ray covers at most.
    F32 maxContactPixels(const LLRHI::GtaoView& view)
    {
        return CONTACT_MAX_SCREEN_SHARE * (F32)view.height;
    }

    F32 smoothStep(F32 edge0, F32 edge1, F32 x)
    {
        const F32 t = llclamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // The diagnostics' line: what the passes do at your avatar's head (its
    // centre). The contact shadow numbers follow llvkgtao.cpp's rayLength.
    void logDiagnostics(const Settings& settings, const LLRHI::GtaoView& view, bool ao, bool contact)
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->mHeadp || !view.height)
        {
            return;
        }
        const LLViewerCamera& camera = *LLViewerCamera::getInstance();
        const LLVector3 offset = gAgentAvatarp->mHeadp->getWorldPosition() - camera.getOrigin();
        const F32 eye[3] = { -(offset * camera.getLeftAxis()), offset * camera.getUpAxis(), -(offset * camera.getAtAxis()) };
        const F32 depth = -eye[2];
        if (depth <= 0.f)
        {
            return;
        }
        const F32 metre = view.projection[1] * 0.5f / depth;     // screen heights
        std::string text = llformat("GTAO diagnostics: head %.2f m deep, a metre there spans %.2f screen heights (%ux%u)", depth,
                                    metre, view.width, view.height);
        F32 light[3];
        if (contact && shadowingLightDirection(light))
        {
            const F32 kx = view.projection[0] * (light[0] * eye[2] - eye[0] * light[2]) * 0.5f * (F32)view.width;
            const F32 ky = view.projection[1] * (light[1] * eye[2] - eye[1] * light[2]) * 0.5f * (F32)view.height;
            const F32 k = std::sqrt(kx * kx + ky * ky);
            const F32 cap = maxContactPixels(view);
            const F32 whole_end = depth - settings.contact_length * light[2];
            const F32 denominator = k + cap * depth * light[2];
            const F32 ray = denominator > 0.f ? llmin(settings.contact_length, cap * depth * depth / denominator) : settings.contact_length;
            const F32 end = depth - ray * light[2];
            const F32 pixels = end > 0.f ? k * ray / (depth * end) : cap;
            const U32 steps = llclamp((U32)std::ceil(pixels), 1u, settings.level.contact_steps);
            const F32 strength = (1.f - smoothStep(CONTACT_FADE_START, CONTACT_FADE_END, depth)) *
                                 (1.f - smoothStep(CONTACT_NEAR_FADE_START, CONTACT_NEAR_FADE_END, metre));
            text += whole_end > 0.f ? llformat("; contact shadows: the %.0f cm ray would cover %.0f pixels", settings.contact_length * 100.f,
                                               k * settings.contact_length / (depth * whole_end))
                                    : llformat("; contact shadows: the %.0f cm ray would pass the eye", settings.contact_length * 100.f);
            text += llformat(", kept to %.1f cm (%.0f pixels, %u steps %.1f pixels apart), strength %.2f", ray * 100.f, pixels, steps,
                             pixels / (F32)steps, strength);
        }
        if (ao)
        {
            const U32 scale = settings.level.scale;
            const F32 height = (F32)((view.height + scale - 1) / scale);
            const F32 full = settings.radius * view.projection[1] * 0.5f * height / depth;
            const F32 kept = llmin(full, MAX_SCREEN_RADIUS_SHARE * height);
            text += llformat("; GTAO: the %.2f m radius would span %.0f pixels (1/%u), kept to %.0f (%.1f cm)", settings.radius, full,
                             scale, kept, full > 0.f ? settings.radius * kept / full * 100.f : 0.f);
        }
        LL_INFOS("PlusGtao") << text << LL_ENDL;
    }

    // Draws GTAO's reduced passes and fills in apply, the pass that writes the
    // light map's g from them; true when apply is ready to draw. When a pass
    // cannot be recorded the light map keeps what the sun program put there.
    bool renderOcclusion(const Settings& settings, const LLRHI::GtaoView& view, LLRHI::GtaoApply& apply)
    {
        const U32 scale = settings.level.scale;
        const U32 width = (view.width + scale - 1) / scale;
        const U32 height = (view.height + scale - 1) / scale;
        const Occlusion source = sizeTargets(width, height, settings.gi);
        if (source == Occlusion::None)
        {
            return false;
        }
        const U32 stop_after = debugPasses();
        if (stop_after == DEBUG_TARGETS_ONLY)
        {
            return false;
        }
        LLRHI::gpuMark("gtao");
        Targets& t = targets();

        LLRHI::GtaoDepth depth;
        depth.view = view;
        depth.scale = scale;
        t.view_depth.bindTarget();
        bool drawn = LLRHI::sGtaoDepth(depth);
        t.view_depth.flush();
        if (!drawn || stop_after == DEBUG_DEPTH_ONLY)
        {
            return false;
        }

        LLRHI::GtaoOcclusion occlusion;
        occlusion.view = view;
        occlusion.view_depth_texture = t.view_depth.getTexture();
        occlusion.width = width;
        occlusion.height = height;
        occlusion.scale = scale;
        occlusion.radius = settings.radius;
        occlusion.max_screen_radius = MAX_SCREEN_RADIUS_SHARE * (F32)height;
        occlusion.slices = settings.level.slices;
        occlusion.steps = settings.level.steps;
        if (source == Occlusion::Ssgi)
        {
            // SSGI's passes, both the occlusion and the 4x4 average.
            const U32 averaged = plus::ssgi::renderOcclusion(occlusion);
            if (!averaged || stop_after == DEBUG_TO_OCCLUSION || stop_after == DEBUG_TO_DENOISE)
            {
                return false;
            }
            apply.view = view;
            apply.occlusion_texture = averaged;
            apply.width = width;
            apply.height = height;
            apply.scale = scale;
            apply.power = stop_after == DEBUG_APPLY_NOTHING ? 0.f : settings.power;
            return true;
        }
        t.occlusion.bindTarget();
        drawn = LLRHI::sGtaoOcclusion(occlusion);
        t.occlusion.flush();
        if (!drawn || stop_after == DEBUG_TO_OCCLUSION)
        {
            return false;
        }

        LLRHI::GtaoDenoise denoise;
        denoise.occlusion_texture = t.occlusion.getTexture();
        denoise.width = width;
        denoise.height = height;
        t.denoised.bindTarget();
        drawn = LLRHI::sGtaoDenoise(denoise);
        t.denoised.flush();
        if (!drawn || stop_after == DEBUG_TO_DENOISE)
        {
            return false;
        }

        apply.view = view;
        apply.occlusion_texture = t.denoised.getTexture();
        apply.width = width;
        apply.height = height;
        apply.scale = scale;
        apply.power = stop_after == DEBUG_APPLY_NOTHING ? 0.f : settings.power;
        return true;
    }

    // Writes the light map's g from GTAO's targets; true when it was written.
    bool applyOcclusion(const LLRHI::GtaoApply& apply)
    {
        LLRenderTarget& light_map = gPipeline.mRT->deferredLight;
        light_map.bindTarget();
        const bool applied = LLRHI::sGtaoApply(apply);
        light_map.flush();
        return applied;
    }

    // The contact shadow pass for this frame; false when there is no light to cast them.
    bool contactShadowsPass(const Settings& settings, const LLRHI::GtaoView& view, LLRHI::ContactShadows& pass)
    {
        if (!shadowingLightDirection(pass.light_direction))
        {
            return false;
        }
        pass.view = view;
        pass.length = settings.contact_length;
        pass.thickness = CONTACT_THICKNESS;
        pass.fade_start = CONTACT_FADE_START;
        pass.fade_end = CONTACT_FADE_END;
        pass.max_screen_length = maxContactPixels(view);
        pass.near_fade_start = CONTACT_NEAR_FADE_START;
        pass.near_fade_end = CONTACT_NEAR_FADE_END;
        pass.steps = settings.level.contact_steps;
        return true;
    }

    // Lowers the light map's r where a contact shadow falls.
    void renderContactShadows(const LLRHI::ContactShadows& pass)
    {
        LLRHI::gpuMark("contact");
        LLRenderTarget& light_map = gPipeline.mRT->deferredLight;
        light_map.bindTarget();
        LLRHI::sContactShadows(pass);
        light_map.flush();
    }

    // Both in one pass (research/125 #9); true when it was drawn. Only when the
    // sun program left g at 1 this frame: the pass keeps the smaller of its g
    // and the light map's (see llrhigtao.h), the apply pass's value only then.
    bool renderApplyAndContact(const LLRHI::GtaoApply& apply, const LLRHI::ContactShadows& contact)
    {
        LLRHI::gpuMark("gtao+contact");
        LLRenderTarget& light_map = gPipeline.mRT->deferredLight;
        light_map.bindTarget();
        const bool drawn = LLRHI::sGtaoApplyContact(apply, contact);
        light_map.flush();
        return drawn;
    }

    // PlusGtaoOnePass (on): GTAO's apply and the contact shadows in one pass
    // when both run. Off draws them apart (the same pixels; for comparing).
    bool onePassWanted()
    {
        static LLCachedControl<bool> one_pass(gSavedSettings, "PlusGtaoOnePass", true);
        return one_pass && LLRHI::sGtaoApplyContact;
    }

    // Settings: app_settings/settings_plus.xml (PlusGtao*, PlusContactShadow*), set in the
    // photo tools (app_settings/plus/phototools/catalog.xml, tabs "quality" and "subject": Quality and Avatar).
    // Drawn from the <plus> hook in LLPipeline::renderDeferredLighting().
    plus::FeatureRegistration sRegistration({ "gtao", nullptr, {} });
}

namespace plus::gtao
{
    namespace
    {
        // render()'s passes; true when the light map's g was written by GTAO.
        // g_is_one: the sun program drew without the official SSAO this frame.
        bool renderPasses(bool g_is_one)
        {
            const LLRenderTarget& screen = gPipeline.mRT->deferredScreen;
            const Settings settings = readSettings(screen.getWidth(), screen.getHeight());
            const bool backend = backendReady();
            if (!settings.ao || !backend)
            {
                releaseTargets();
            }
            if (!settings.ao)
            {
                // turning GTAO off and on tries leaving the official SSAO out again at once
                sWaitingToRetry = false;
                sRetryWait = RETRY_FIRST_WAIT;
            }
            if (!backend || (!settings.ao && !settings.contact) || !mainLightMapReady())
            {
                return false;
            }
            // The light map's g counts only with the official SSAO (softenLight's HAS_SSAO),
            // its r only with sun shadows (HAS_SUN_SHADOW).
            const bool ao = settings.ao && LLPipeline::RenderDeferredSSAO;
            const bool contact = settings.contact && LLPipeline::RenderShadowDetail > 0;
            if (!ao)
            {
                releaseTargets();       // nothing would read them
            }
            if (!ao && !contact)
            {
                return false;
            }

            // The projection renderDeferredLighting's own passes use.
            const LLRHI::GtaoView view = viewOf(get_current_projection());
            LLRHI::GtaoApply apply;
            const bool apply_ready = ao && renderOcclusion(settings, view, apply);
            LLRHI::ContactShadows contact_pass;
            const bool contact_ready = contact && contactShadowsPass(settings, view, contact_pass);
            bool occlusion_drawn = false;
            bool contact_drawn = false;
            if (apply_ready && contact_ready && g_is_one && onePassWanted())
            {
                occlusion_drawn = contact_drawn = renderApplyAndContact(apply, contact_pass);
            }
            if (apply_ready && !occlusion_drawn)
            {
                occlusion_drawn = applyOcclusion(apply);
            }
            if (contact_ready && !contact_drawn)
            {
                renderContactShadows(contact_pass);
            }
            LLRHI::gpuMark("lighting");     // the GPU ms line: the rest of the lighting goes back to its segment

            static LLCachedControl<bool> diagnostics(gSavedSettings, SETTING_DIAGNOSTICS, false);
            static F32 logged_at = 0.f;
            if (diagnostics && gFrameTimeSeconds - logged_at >= DIAGNOSTICS_INTERVAL)
            {
                logged_at = gFrameTimeSeconds;
                logDiagnostics(settings, view, ao, contact);
            }
            return occlusion_drawn;
        }
    }

    // GTAO's apply pass writes g on every pixel of the light map (a full-target
    // triangle, colour write mask G, no blending, no discard: llvkgtao.cpp), so
    // whatever g the sun program left is never read. The checks below are the
    // ones renderPasses() and renderOcclusion() make before they reach the
    // apply pass, except that the light map is bound right now (the pipeline
    // binds it before it picks the program); render() checks the rest.
    bool replacesAmbientOcclusion()
    {
        sSunWithoutSsao = false;
        // Not while a photo is drawn: should GTAO fail there (a large snapshot
        // short of VRAM may fail only when the passes are recorded), the saved
        // picture would have no occlusion at all. The official SSAO costs a few
        // tenths of a millisecond once (research/131 M1).
        if (keepingOfficialSsao() || gSnapshot || !LLPipeline::RenderDeferredSSAO || !backendReady() || gCubeSnapshot ||
            gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        if (!rt.deferredLight.isComplete() || !rt.deferredScreen.getDepth() ||
            rt.deferredLight.getWidth() != rt.deferredScreen.getWidth() ||
            rt.deferredLight.getHeight() != rt.deferredScreen.getHeight())
        {
            return false;
        }
        // Debug passes 1, 2, 3 and 5 stop before the apply pass and leave the
        // official SSAO in g; 4 applies 1 everywhere.
        const U32 stop_after = debugPasses();
        if (stop_after != DEBUG_ALL && stop_after != DEBUG_APPLY_NOTHING)
        {
            return false;
        }
        const Settings settings = readSettings(rt.deferredScreen.getWidth(), rt.deferredScreen.getHeight());
        if (!settings.ao)
        {
            return false;
        }
        // The GTAO targets now, not after the sun program: when they cannot be
        // made (short of VRAM) this frame keeps the official SSAO instead of
        // losing its occlusion (research/131 M1). Making them
        // while the light map is bound is safe: LLRenderTarget::allocate
        // puts the bound framebuffer back. renderOcclusion() finds them made.
        const U32 scale = settings.level.scale;
        if (sizeTargets((rt.deferredScreen.getWidth() + scale - 1) / scale, (rt.deferredScreen.getHeight() + scale - 1) / scale,
                        settings.gi) == Occlusion::None)
        {
            return false;
        }
        sSunWithoutSsao = true;
        return true;
    }

    const char* unavailableReason()
    {
        if (!LLRHI::sGtao)
        {
            return plus::safemode::active() ? reason::SAFE_MODE : reason::GTAO_SWITCHED_OFF;
        }
        if (!backendReady())
        {
            return reason::NO_BACKEND;
        }
        if (!LLPipeline::RenderDeferredSSAO)
        {
            // where the feature table has no SSAO, its check box is greyed out too
            static const bool in_table = LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferredSSAO");
            return in_table ? reason::SSAO_OFF : reason::SSAO_UNSUPPORTED;
        }
        return nullptr;
    }

    bool available()
    {
        return !unavailableReason();
    }

    void render()
    {
        const bool sun_without_ssao = sSunWithoutSsao;
        sSunWithoutSsao = false;
        const bool occlusion_drawn = renderPasses(sun_without_ssao);
        if (!sun_without_ssao)
        {
            return;
        }
        if (occlusion_drawn)
        {
            sRetryWait = RETRY_FIRST_WAIT;
            return;
        }
        // This frame has no ambient occlusion at all; for a while the sun
        // program computes the official SSAO again, as before.
        sWaitingToRetry = true;
        sRetryFrame = LLFrameTimer::getFrameCount() + sRetryWait;
        LL_WARNS("PlusGtao") << "GTAO was not drawn after the sun program left out the official SSAO; "
                             << "the official SSAO is computed again for " << sRetryWait << " frames" << LL_ENDL;
        sRetryWait = llmin(sRetryWait * 2, RETRY_MAX_WAIT);
    }
}
