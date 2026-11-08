/**
 * @file ssr.cpp
 * @brief Screen space reflections in passes of their own: a port of the
 *        official project/graphics-care-package's (research/129). Registers
 *        with the plus foundation.
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

// The official viewer traces screen space reflections inside the lighting
// shaders: a linear march, four rays a pixel, every frame
// (screenSpaceReflUtil.glsl), on PBR surfaces with a roughness of 0.1 or
// less. Linden Lab's feature branch project/graphics-care-package (GCP; not
// in a release yet) traces the opaque surfaces in passes of their own
// instead, through a pyramid of the depth (Hi-Z: long rays in few steps),
// one ray a pixel each frame added up over the frames (temporal
// accumulation), blurs the result by the roughness and lets the lighting
// read it, up to a roughness of 0.7. This ports that (research/129 lists what was taken, what
// was finished here and what was left).
//
// PlusSSR (on by default, and at every quality level but MidHigh and under
// the GPU masks that cap it) switches it on, over the official
// RenderScreenSpaceReflections (off by default and below High: PlusSSR does
// nothing without it; the scene copy the rays look into is the official one). Vulkan only: the passes are recorded
// by the backend (llvulkan/llvkssr.cpp). Glass, other alpha and water keep
// the official tracing (GCP's passes for them draw that geometry again).
// The GCP settings this reads keep GCP's names (settings_plus.xml for now:
// drop them there when GCP's settings.xml arrives).
//
// Each frame, before the lighting, on the main view:
//  1. The depth pyramid of the scene copy (the last frame's depth).
//  2. The trace: per pixel of a glossy surface (this frame's G-buffer), rays
//     along the reflection, GGX-jittered by the roughness, carried into the
//     last frame's view and traced through the pyramid; the scene copy's
//     colour where one hits. With temporal accumulation
//     (RenderScreenSpaceReflectionTemporal, on) one ray, its noise turned
//     each frame; without it RenderScreenSpaceReflectionGlossySamples rays
//     in the same pattern every frame.
//  3. The resolve (temporal accumulation): the trace and the last frame's
//     result, moved with the camera and clipped to the trace's spread
//     around the pixel (with PlusSSRReflectionMotion, on: taken where the
//     reflection was a frame ago rather than the surface, from the hits'
//     distance the trace also writes; research/294); none of it where the
//     surface was hidden a frame ago, after a jump of the camera (teleport,
//     region crossing), a skipped
//     frame or a size change, and none in snapshots (they trace several
//     rays instead). On avatars (the avatar bit of plus/skinsss's mark, made
//     into a mask first) only PlusSSRAvatarHistory of it: their skin and
//     clothes move where the camera's motion does not take them. GCP's
//     velocity buffer (every draw pool drawn again with the last frame's
//     matrices and skinning) is not ported: research/129.
//  4. The filter: the result blurred by each pixel's roughness (a pyramid of
//     it), faded out towards the roughness limit.
// The lighting (reflectionProbeF.glsl under PLUS_SSR) reads that in place of
// tracing, upsampled with the depth as the guide when it is smaller than the
// screen.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "ssr.h"

#include "avatarrefl/avatarrefl.h"
#include "skinsss/skinsss.h"

#include "llcallbacklist.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhissr.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_ENABLED[]        = "PlusSSR";
    constexpr char SETTING_ITERATIONS[]     = "PlusSSRIterations";
    constexpr char SETTING_AVATAR_HISTORY[] = "PlusSSRAvatarHistory";
    constexpr char SETTING_REFLECTION_MOTION[] = "PlusSSRReflectionMotion";
    constexpr char SETTING_CLIP_SIGMA[]     = "PlusSSRClipSigma";
    // GCP's names (its settings.xml).
    constexpr char SETTING_MAX_THICKNESS[]  = "RenderScreenSpaceReflectionMaxThickness";
    constexpr char SETTING_DEPTH_BIAS[]     = "RenderScreenSpaceReflectionDepthBias";
    constexpr char SETTING_MAX_DEPTH[]      = "RenderScreenSpaceReflectionMaxDepth";
    constexpr char SETTING_MAX_ROUGHNESS[]  = "RenderScreenSpaceReflectionMaxRoughness";
    constexpr char SETTING_RESOLUTION[]     = "RenderScreenSpaceReflectionsResolutionMultiplier";
    constexpr char SETTING_BILATERAL[]      = "RenderScreenSpaceReflectionsBilateralUpsample";
    constexpr char SETTING_FILTER_SCALE[]   = "RenderScreenSpaceReflectionsFilterScale";
    constexpr char SETTING_TEMPORAL[]       = "RenderScreenSpaceReflectionTemporal";
    constexpr char SETTING_TEMPORAL_BLEND[] = "RenderScreenSpaceReflectionTemporalBlend";

    constexpr F32 MIN_RESOLUTION = 0.25f;   // GCP's clamp of the resolution multiplier
    constexpr U32 MAX_ITERATIONS = 256;
    constexpr F32 MIN_TEMPORAL_BLEND = 0.5f;    // GCP's clamp of the history's weight
    constexpr F32 MAX_TEMPORAL_BLEND = 0.98f;
    // The history is dropped where the surface's depth a frame ago differs
    // from the scene copy's by more than this share (all of it at twice this).
    constexpr F32 DISOCCLUSION = 0.05f;
    // A camera that moved further than this in a frame jumped (a teleport,
    // the agent frame moving to another region): no history.
    constexpr F32 JUMP_METRES = 10.f;
    // The noise's phase wraps at this many frames.
    constexpr U32 NOISE_PERIOD = 1024;

    struct Settings
    {
        U32 iterations = 48;
        U32 rays = 4;
        F32 max_thickness = 1.f;
        F32 depth_bias = 0.f;
        F32 max_depth = 256.f;
        F32 max_roughness = 0.7f;
        F32 resolution = 0.5f;
        bool bilateral = true;
        F32 filter_scale = 2.f;
        bool temporal = true;
        // temporal but for snapshots: the trace's target keeps its make through one (research/294 review L5).
        bool temporal_outside_snapshots = true;
        F32 temporal_blend = 0.9f;
        F32 avatar_history = 0.5f;
        bool reflection_motion = true;      // with temporal: the history follows the reflection (research/294 S1, S2)
        F32 clip_sigma = LLRHI::SSR_CLIP_SIGMA_DEFAULT;
    };

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ENABLED, true);
        static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
        // Without reflection probes nothing reads the reflections (bindReflectionProbes returns at once);
        // without HDR renderFinalize never copies the scene the trace reads (copyScreenSpaceReflections).
        return on && LLPipeline::RenderScreenSpaceReflections && LLPipeline::sReflectionProbesEnabled && LLRHI::sSsr &&
               gGLManager.mGLVersion > 4.05f && has_hdr();
    }

    bool backendReady()
    {
        return LLRHI::sSsrHiZ && LLRHI::sSsrTrace && LLRHI::sSsrFilter;    // sSsrResolve: temporal accumulation only
    }

    Settings readSettings()
    {
        static LLCachedControl<S32> iterations(gSavedSettings, SETTING_ITERATIONS, 48);
        static LLCachedControl<F32> max_thickness(gSavedSettings, SETTING_MAX_THICKNESS, 1.f);
        static LLCachedControl<F32> depth_bias(gSavedSettings, SETTING_DEPTH_BIAS, 0.f);
        static LLCachedControl<F32> max_depth(gSavedSettings, SETTING_MAX_DEPTH, 256.f);
        static LLCachedControl<F32> max_roughness(gSavedSettings, SETTING_MAX_ROUGHNESS, 0.7f);
        static LLCachedControl<F32> resolution(gSavedSettings, SETTING_RESOLUTION, 0.5f);
        static LLCachedControl<bool> bilateral(gSavedSettings, SETTING_BILATERAL, true);
        static LLCachedControl<F32> filter_scale(gSavedSettings, SETTING_FILTER_SCALE, 2.f);
        static LLCachedControl<bool> temporal(gSavedSettings, SETTING_TEMPORAL, true);
        static LLCachedControl<F32> temporal_blend(gSavedSettings, SETTING_TEMPORAL_BLEND, 0.9f);
        static LLCachedControl<F32> avatar_history(gSavedSettings, SETTING_AVATAR_HISTORY, 0.5f);
        static LLCachedControl<bool> reflection_motion(gSavedSettings, SETTING_REFLECTION_MOTION, true);
        static LLCachedControl<F32> clip_sigma(gSavedSettings, SETTING_CLIP_SIGMA, LLRHI::SSR_CLIP_SIGMA_DEFAULT);
        Settings out;
        out.iterations = (U32)llclamp((S32)iterations, 1, (S32)MAX_ITERATIONS);
        // Several rays a pixel every frame: the official setting's (the same meaning).
        out.rays = (U32)llclamp(LLPipeline::RenderScreenSpaceReflectionGlossySamples, 1, 16);
        out.max_thickness = llmax((F32)max_thickness, 0.001f);
        out.depth_bias = llmax((F32)depth_bias, 0.f);
        out.max_depth = llmax((F32)max_depth, 1.f);
        // NaN would pass llclamp() into the shaders and the buffers' size (llmax() already drops it).
        out.max_roughness = llfinite((F32)max_roughness) ? llclamp((F32)max_roughness, 0.f, 1.f) : 0.7f;
        out.resolution = llfinite((F32)resolution) ? llclamp((F32)resolution, MIN_RESOLUTION, 1.f) : 0.5f;
        out.bilateral = bilateral;
        out.filter_scale = llmax((F32)filter_scale, 0.f);
        // A snapshot is drawn once (in tiles, at another size): no history to add up.
        out.temporal_outside_snapshots = temporal && LLRHI::sSsrResolve;
        out.temporal = out.temporal_outside_snapshots && !gSnapshot;
        out.temporal_blend = llfinite((F32)temporal_blend) ? llclamp((F32)temporal_blend, MIN_TEMPORAL_BLEND, MAX_TEMPORAL_BLEND) : 0.9f;
        out.avatar_history = llfinite((F32)avatar_history) ? llclamp((F32)avatar_history, 0.f, 1.f) : 0.5f;
        out.reflection_motion = reflection_motion;
        out.clip_sigma = llfinite((F32)clip_sigma) ? llclamp((F32)clip_sigma, LLRHI::SSR_CLIP_SIGMA_MIN, LLRHI::SSR_CLIP_SIGMA_MAX)
                                                           : LLRHI::SSR_CLIP_SIGMA_DEFAULT;
        return out;
    }

    // ------------------------------------------------------------------
    // The SSR buffers, at the resolution multiplier's share of the screen.
    // Made on first use and kept while the feature is on; never destroyed by
    // a static destructor, which would run without a GL context.

    struct Targets
    {
        LLRenderTarget trace;       // RGBA16F: the trace (premultiplied, compressed colour; the weight);
                                    // with the reflection's motion also R16F: the hits' distance (m)
        LLRenderTarget resolved[2]; // RGBA16F: the temporal accumulation, this frame's and the last one's in turn
        LLRenderTarget output;      // RGBA16F: blurred by the roughness, what the lighting reads
        LLRenderTarget avatar_mask; // R8 at the G-buffer's size, sharing its depth: 1 on avatars
        U32 mask_generation = 0;    // the screen buffers it shares the depth of
        // After one could not be made (VRAM full): not again every frame.
        plus::RetryWait retry;          // trace and output
        plus::RetryWait hit_retry;      // the trace's hits' distance (its second attachment)
        plus::RetryWait history_retry;  // resolved[]
        plus::RetryWait mask_retry;     // avatar_mask
    };

    Targets& targets()
    {
        static Targets* targets = new Targets();
        return *targets;
    }

    struct State
    {
        bool live = false;              // output holds this frame's reflections
        U32 live_frame = 0;             // gFrameCount it was drawn in
        F32 res_scale = 1.f;            // output's width / the screen's, for the lighting's upsampling (1: none)
        glm::mat4 prev_projection = glm::mat4(1.f);     // the projection of the frame the scene copy holds
        bool has_prev = false;
        bool backend_images = false;    // the backend may hold pyramids (released with an empty SsrHiZ)
        U32 resolved_index = 0;         // resolved[] this frame writes (the other is the history)
        U32 history_frame = 0;          // gFrameCount of the last resolve (0: none)
        // The scene copy as the last frame found it: a copy made anew (a size
        // change, a snapshot's buffers, SSR just on) holds nothing until the
        // end of its first frame, so that frame traces nothing.
        U32 scene_color = 0;
        U32 scene_depth = 0;
        U32 scene_width = 0;
        U32 scene_height = 0;
        U32 logged_width = 0;
        U32 logged_height = 0;
        bool logged_follows_reflection = false;
    };

    State& state()
    {
        static State s;
        return s;
    }

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
    }

    void releaseHistory()
    {
        Targets& t = targets();
        releaseTarget(t.resolved[0]);
        releaseTarget(t.resolved[1]);
        releaseTarget(t.avatar_mask);
        state().history_frame = 0;
    }

    void release()
    {
        Targets& t = targets();
        releaseTarget(t.trace);
        releaseTarget(t.output);
        releaseHistory();
        State& s = state();
        if (s.backend_images && LLRHI::sSsrHiZ)
        {
            LLRHI::sSsrHiZ(LLRHI::SsrHiZ());    // no depth: the backend drops its pyramids
        }
        s.backend_images = false;
        s.has_prev = false;
        s.live = false;
        s.logged_width = s.logged_height = 0;
        s.logged_follows_reflection = false;
        s.scene_color = s.scene_depth = s.scene_width = s.scene_height = 0;
    }

    // made: set when the target was (re)allocated (what it held is gone).
    // retry: the wait shared by the targets made together. second_format: a
    // second attachment's format (GL_R16F), or 0 for none; a target with
    // another number of attachments is made anew.
    bool sizeTarget(LLRenderTarget& target, U32 width, U32 height, U32 second_format, plus::RetryWait& retry, bool* made = nullptr)
    {
        const U32 attachments = second_format ? 2 : 1;
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height && target.getNumTextures() == attachments)
        {
            return true;
        }
        if (!retry.due())
        {
            return false;       // one could not be made a moment ago
        }
        if (made)
        {
            *made = true;
        }
        if (!target.allocate(width, height, GL_RGBA16F) || (second_format && !target.addColorAttachment(second_format)))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            retry.failed();
            return false;
        }
        retry.succeeded();
        // Bilinear (the blur pyramid and the lighting sample it); unit 0 left empty.
        target.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR);
        if (second_format)
        {
            target.bindTexture(1, 0, LLTexUnit::TFO_POINT);     // read texel by texel
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // The output's and the trace's targets. hit_distance: asked for the trace's
    // target with the hits' distance too (R16F; let go without it); false on
    // return when that could not be made (a moment ago), the trace's target
    // then made as before research/294. False when not even that could be
    // made. The output comes first: while it cannot be made, the trace's
    // target is not made (and let go by the caller) again each frame.
    bool sizeTargets(U32 width, U32 height, bool& hit_distance)
    {
        Targets& t = targets();
        if (!sizeTarget(t.output, width, height, 0, t.retry))
        {
            return false;
        }
        if (hit_distance)
        {
            const bool tried = t.hit_retry.due();   // false: still waiting after a failure (no new attempt)
            if (!sizeTarget(t.trace, width, height, GL_R16F, t.hit_retry))
            {
                hit_distance = false;
                if (tried)
                {
                    LL_WARNS("PlusSSR") << "SSR: no hit distance buffer of " << width << "x" << height
                                        << "; the history follows the surface (tried again in "
                                        << plus::RetryWait::RETRY_SECONDS << " s)" << LL_ENDL;
                }
            }
        }
        return hit_distance || sizeTarget(t.trace, width, height, 0, t.retry);
    }

    // The temporal accumulation's two buffers; false (and none) when they could not be made.
    bool sizeHistory(U32 width, U32 height)
    {
        Targets& t = targets();
        bool made = false;
        if (!sizeTarget(t.resolved[0], width, height, 0, t.history_retry, &made) ||
            !sizeTarget(t.resolved[1], width, height, 0, t.history_retry, &made))
        {
            releaseHistory();
            return false;
        }
        if (made)
        {
            state().history_frame = 0;  // nothing to add up yet
        }
        return true;
    }

    // For plus::skinsss::addAvatarMarkUser: avatars told from the rest while
    // the frames add up and avatars take less of the history.
    bool avatarMarkWanted()
    {
        static LLCachedControl<bool> temporal(gSavedSettings, SETTING_TEMPORAL, true);
        static LLCachedControl<F32> avatar_history(gSavedSettings, SETTING_AVATAR_HISTORY, 0.5f);
        return enabled() && backendReady() && temporal && avatar_history < 1.f && LLRHI::sSsrResolve && LLRHI::sSsrAvatarMask;
    }

    // The avatar mask at the size of gbuffer, sharing its depth (its stencil
    // holds the mark); false when it could not be made.
    bool prepareAvatarMask(LLRenderTarget& gbuffer)
    {
        Targets& t = targets();
        const U32 width = gbuffer.getWidth();
        const U32 height = gbuffer.getHeight();
        const U32 generation = plus::skinsss::screenBuffersGeneration();
        if (t.avatar_mask.isComplete() && t.mask_generation == generation && t.avatar_mask.getWidth() == width &&
            t.avatar_mask.getHeight() == height)
        {
            return true;
        }
        if (!t.mask_retry.due())
        {
            return false;       // it could not be made a moment ago: not again every frame
        }
        releaseTarget(t.avatar_mask);
        if (!t.avatar_mask.allocate(width, height, GL_R8))
        {
            t.avatar_mask.release();    // a half-made target is not isComplete(): releaseTarget would skip it
            t.mask_retry.failed();
            LL_WARNS_ONCE("PlusSSR") << "SSR: no avatar mask of " << width << "x" << height << LL_ENDL;
            return false;
        }
        t.mask_retry.succeeded();
        t.avatar_mask.bindTexture(0, 0, LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gbuffer.shareDepthBuffer(t.avatar_mask);
        t.mask_generation = generation;
        return true;
    }

    // Whether the last frame's resolve is this frame's history: drawn the
    // frame before (a gap is a teleport, the login, a minimized window), and
    // the camera did not jump.
    bool historyUsable()
    {
        const State& s = state();
        if (!s.history_frame || s.history_frame + 1 != gFrameCount)
        {
            return false;
        }
        const glm::vec3 moved(gGLDeltaModelView[3]);
        return glm::length(moved) <= JUMP_METRES;
    }

    void copyMatrix(F32 (&out)[16], const glm::mat4& m)
    {
        memcpy(out, glm::value_ptr(m), sizeof(out));
    }

    // The camera and the G-buffer for the passes. projection: this frame's;
    // prev_projection: the frame the scene copy holds.
    LLRHI::SsrFrame makeFrame(const LLRenderTarget& gbuffer, const glm::mat4& projection, const glm::mat4& prev_projection)
    {
        LLRHI::SsrFrame frame;
        copyMatrix(frame.projection, projection);
        copyMatrix(frame.inverse_projection, glm::inverse(projection));
        copyMatrix(frame.prev_projection, prev_projection);
        copyMatrix(frame.inverse_prev_projection, glm::inverse(prev_projection));
        // This frame's eye space -> the last frame's (renderGeomDeferred set it for the main camera).
        copyMatrix(frame.inverse_modelview_delta, gGLInverseDeltaModelView);
        frame.depth_texture = gbuffer.getDepth();
        frame.specular_texture = gbuffer.getTexture(1);
        frame.normal_texture = gbuffer.getTexture(2);
        frame.width = gbuffer.getWidth();
        frame.height = gbuffer.getHeight();
        return frame;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusSSR*, and GCP's
        // RenderScreenSpaceReflection* that the official settings.xml does
        // not have yet). Drawn from the <plus> hooks in
        // LLPipeline::renderDeferredLighting() and bindReflectionProbes().
        plus::skinsss::addAvatarMarkUser(&avatarMarkWanted);
        // The avatar mark comes and goes with the settings. Asked once idle,
        // when the cached controls have the new values.
        for (const char* setting : { SETTING_ENABLED, "RenderScreenSpaceReflections", SETTING_TEMPORAL, SETTING_AVATAR_HISTORY })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(setting))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                              { doOnIdleOneTime(&plus::skinsss::markWantChanged); });
            }
        }
    }

    plus::FeatureRegistration sRegistration({ "ssr", &init, {} });
}

namespace plus::ssr
{
    void addShaderDefines(std::map<std::string, std::string>& attribs)
    {
        // Always with the official SSR (the composite is chosen per program
        // by plusSSRParams): turning PlusSSR on or off needs no rebuild.
        if (attribs.count("SSR"))
        {
            attribs["PLUS_SSR"] = "1";
        }
    }

    void render()
    {
        State& s = state();
        if (!enabled() || !backendReady())
        {
            release();
            return;
        }
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;     // a probe face or mirror: the main view's buffer is left as it is
        }
        s.live = false;

        // The scene copy is made at the end of each main frame (renderFinalize):
        // the projection it was drawn with is this one, for the next frame.
        const glm::mat4 projection = get_current_projection();
        const glm::mat4 prev_projection = s.has_prev ? s.prev_projection : projection;
        s.prev_projection = projection;
        s.has_prev = true;

        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        LLRenderTarget& scene = gPipeline.mSceneMap;
        if (!gbuffer.isComplete() || !gbuffer.getDepth() || gbuffer.getNumTextures() < 3 || gbuffer.isBoundInStack() ||
            !scene.isComplete() || !scene.getDepth() || scene.isBoundInStack())
        {
            return;
        }
        if (scene.getTexture() != s.scene_color || scene.getDepth() != s.scene_depth || scene.getWidth() != s.scene_width ||
            scene.getHeight() != s.scene_height)
        {
            // A new scene copy: undefined until this frame's end copies into it.
            s.scene_color = scene.getTexture();
            s.scene_depth = scene.getDepth();
            s.scene_width = scene.getWidth();
            s.scene_height = scene.getHeight();
            s.history_frame = 0;
            return;
        }

        Settings settings = readSettings();
        const U32 width = llmax(1u, (U32)std::lround(gbuffer.getWidth() * settings.resolution));
        const U32 height = llmax(1u, (U32)std::lround(gbuffer.getHeight() * settings.resolution));
        // The hits' distance only feeds the resolve: without it, the trace's target as before research/294.
        // Decided before the history's buffers (which a failed trace target would let go again each
        // frame) and through snapshots (no target made anew around each): while there is no resolve
        // the trace writes it unread. The backend's flag is false until its passes are set up. While the
        // history's buffers cannot be made (waiting to try again) it is let go too (no unread buffer kept).
        Targets& t = targets();
        bool hit_distance = settings.temporal_outside_snapshots && settings.reflection_motion && LLRHI::sSsrHitDistance &&
                            (t.resolved[0].isComplete() || t.history_retry.due());
        if (!sizeTargets(width, height, hit_distance))
        {
            LL_WARNS_ONCE("PlusSSR") << "SSR: no buffers of " << width << "x" << height << LL_ENDL;
            release();
            return;
        }
        if (settings.temporal && !sizeHistory(width, height))
        {
            // Without the history's buffers: one frame at a time.
            LL_WARNS_ONCE("PlusSSR") << "SSR: no temporal accumulation buffers of " << width << "x" << height << LL_ENDL;
            settings.temporal = false;
        }
        if (!settings.temporal)
        {
            releaseHistory();
        }
        const bool follows_reflection = settings.temporal && hit_distance;     // what the resolve does this frame
        const bool way_changed = s.logged_follows_reflection != follows_reflection;
        if (s.logged_width != width || s.logged_height != height || way_changed)
        {
            s.logged_width = width;
            s.logged_height = height;
            s.logged_follows_reflection = follows_reflection;
            static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
            const U32 detail = (settings.temporal ? 1 : 0) + (follows_reflection ? 2 : 0);
            LLError::ELevel log_level = resize_log.level(width, height, detail);    // once: LL_VLOGS reads its level twice
            if (way_changed)
            {
                // A switch of the resolve's way (settings, buffers, the backend's passes ready a frame after
                // the first): not a dragged frame, always said (the second frame's would fall in the throttle).
                log_level = LLError::LEVEL_INFO;
            }
            LL_VLOGS(log_level, "PlusSSR") << "SSR (GCP port): " << width << "x" << height << " for a " << gbuffer.getWidth() << "x"
                                << gbuffer.getHeight() << " screen, " << (settings.temporal ? "temporal accumulation" : "one frame at a time")
                                << (follows_reflection ? " following the reflection" : "") << LL_ENDL;
        }

        LLRHI::gpuMark("ssr");
        gGL.flush();
        const LLRHI::SsrFrame frame = makeFrame(gbuffer, projection, prev_projection);

        LLRHI::SsrHiZ hiz;
        hiz.scene_depth_texture = scene.getDepth();
        hiz.width = scene.getWidth();
        hiz.height = scene.getHeight();
        s.backend_images = true;
        bool drawn = LLRHI::sSsrHiZ(hiz);

        if (drawn)
        {
            LLRHI::SsrTrace trace;
            trace.frame = frame;
            trace.scene_color_texture = scene.getTexture();
            trace.scene_depth_texture = scene.getDepth();
            trace.scene_width = scene.getWidth();
            trace.scene_height = scene.getHeight();
            trace.width = width;
            trace.height = height;
            trace.iterations = settings.iterations;
            // GCP's temporal path still traced RenderScreenSpaceReflectionGlossySamples
            // rays (its resolve says one a frame): one here, the frames add them up.
            trace.rays = settings.temporal ? 1 : settings.rays;
            // Turned each frame while the frames add up, else the same pattern every frame.
            trace.noise = settings.temporal ? (F32)(gFrameCount % NOISE_PERIOD) : 0.f;
            trace.max_thickness = settings.max_thickness;
            trace.depth_bias = settings.depth_bias;
            trace.max_depth = settings.max_depth;
            trace.max_roughness = settings.max_roughness;
            // Your reflection in the floor shows you there: no second, blurred you from the scene copy (research/210 A2).
            plus::avatarrefl::selfSkip(trace.self_box, trace.self_plane, trace.self_params);
            trace.hit_distance = hit_distance;
            t.trace.bindTarget();
            drawn = LLRHI::sSsrTrace(trace);
            t.trace.flush();
        }

        LLRenderTarget* result = &t.trace;
        if (drawn && settings.temporal)
        {
            const bool history = historyUsable();
            s.resolved_index ^= 1;      // the last frame's result becomes the history
            LLRenderTarget& resolved = t.resolved[s.resolved_index];
            LLRHI::SsrResolve resolve;
            resolve.frame = frame;
            resolve.trace_texture = t.trace.getTexture();
            resolve.history_texture = t.resolved[s.resolved_index ^ 1].getTexture();
            resolve.scene_depth_texture = scene.getDepth();
            resolve.scene_width = scene.getWidth();
            resolve.scene_height = scene.getHeight();
            resolve.width = width;
            resolve.height = height;
            resolve.history_blend = history ? settings.temporal_blend : 0.f;
            resolve.disocclusion = DISOCCLUSION;
            resolve.avatar_history = settings.avatar_history;
            resolve.hit_distance_texture = hit_distance ? t.trace.getTexture(1) : 0;
            resolve.clip_sigma = settings.clip_sigma;
            // Avatars told apart only when this frame's G-buffer pass wrote the mark.
            if (history && settings.avatar_history < 1.f && LLRHI::sSsrAvatarMask && plus::skinsss::avatarsMarkedThisFrame() &&
                prepareAvatarMask(gbuffer))
            {
                LLRHI::SsrAvatarMask mask;
                mask.width = gbuffer.getWidth();
                mask.height = gbuffer.getHeight();
                t.avatar_mask.bindTarget();
                if (LLRHI::sSsrAvatarMask(mask))
                {
                    resolve.avatar_mask_texture = t.avatar_mask.getTexture();
                }
                t.avatar_mask.flush();
            }
            resolved.bindTarget();
            drawn = LLRHI::sSsrResolve(resolve);
            resolved.flush();
            s.history_frame = drawn ? gFrameCount : 0;
            result = &resolved;
        }
        else
        {
            s.history_frame = 0;
        }

        if (drawn)
        {
            LLRHI::SsrFilter filter;
            filter.frame = frame;
            filter.source_texture = result->getTexture();
            filter.width = width;
            filter.height = height;
            filter.filter_scale = settings.filter_scale;
            filter.max_roughness = settings.max_roughness;
            t.output.bindTarget();
            drawn = LLRHI::sSsrFilter(filter);
            t.output.flush();
        }

        s.live = drawn;
        s.live_frame = gFrameCount;
        s.res_scale = (settings.bilateral && width < gbuffer.getWidth()) ? (F32)width / (F32)gbuffer.getWidth() : 1.f;
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest of the lighting goes back to its segment
    }

    void bind(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_params("plusSSRParams");
        if (shader.getUniformLocation(s_params) < 0)
        {
            return;     // a program without PLUS_SSR (or one that does not read reflections)
        }
        const State& s = state();
        Targets& t = targets();
        // Only the main view's opaque lighting: the buffer is its pixels'.
        const bool live = s.live && s.live_frame == gFrameCount && &shader == &gDeferredSoftenProgram && !gCubeSnapshot &&
                          gPipeline.mRT == &gPipeline.mMainRT && t.output.isComplete();
        const S32 channel = live ? shader.getTextureChannel(LLShaderMgr::SCENE_MAP) : -1;
        if (channel < 0)
        {
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);    // the official tracing, against the scene copy
            return;
        }
        // In place of the scene copy (which only the official tracing reads).
        gGL.getTexUnit(channel)->bind(&t.output);
        shader.uniform4f(s_params, 1.f, s.res_scale, 0.f, 0.f);
    }
}
