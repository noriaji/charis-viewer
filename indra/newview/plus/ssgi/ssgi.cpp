/**
 * @file ssgi.cpp
 * @brief Screen space indirect light (SSGI): GTAO's occlusion widened into a
 *        visibility bitmask that also gathers the light near occluders send
 *        back (research/164). Registers with the plus foundation.
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

// PlusSsgi (off by default). Vulkan only: the passes are recorded by the
// backend (llvulkan/llvkssgi.cpp); with GL nothing is drawn.
//
// The official lighting (softenLightF.glsl) takes the ambient light from the
// reflection probes and darkens it by the light map's g (ambient occlusion,
// GTAO's while plus/gtao runs). This adds, after that darkening and without
// it, the light that the surfaces near a pixel send back to it: where GTAO
// darkens a corner, the colour of the sunny ground beside it comes back.
// Stage 1 of research/164 (no temporal accumulation, so no ghosting):
//  1. At the end of each main frame's opaque lighting: the lit screen at half
//     size, the sky left out, held at a ceiling (capture()).
//  2. The next frame, inside GTAO's passes (plus/gtao): that light moved into
//     this frame by the camera's motion; then, in place of GTAO's occlusion
//     pass, a visibility bitmask (Therrien et al. 2023) over the same slices
//     and samples: its ambient occlusion goes where GTAO's would, and the
//     light of the sectors each sample covers first is gathered with it; the
//     two averaged over 4x4 together (renderOcclusion()).
//  3. The lighting adds that light to the probes' (bind()).
// The light is the last frame's, the lighting's own result: what it adds is
// in it the next frame (the bounces add up over a few frames). The strength
// stays at 1 at most, so each round gives back less than it took (research/164
// section 4.1, decided 2026-10-25): it settles, it does not run away.
//
// Needs GTAO (PlusGtao) and with it the official SSAO, HDR (without it the
// lighting's range differs and the ceiling does not hold) and the
// reflection probes (the light off the screen and the sky's are theirs).
// Not on probe faces and mirrors (the probes would hold it twice).

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "ssgi.h"

#include "gtao/gtao.h"
#include "safemode/safemode.h"

#include "llappviewer.h"
#include "llenvironment.h"
#include "llfeaturemanager.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhissgi.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "v3color.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot (photo) is being drawn

namespace
{
    constexpr char SETTING_ENABLED[]   = "PlusSsgi";
    constexpr char SETTING_STRENGTH[]  = "PlusSsgiStrength";
    constexpr char SETTING_RADIUS[]    = "PlusSsgiRadius";
    constexpr char SETTING_THICKNESS[] = "PlusSsgiThickness";
    constexpr char SETTING_DEBUG[]     = "PlusSsgiDebug";

    // PlusSsgiDebug (not saved): views to look at what the passes do.
    enum Debug : U32
    {
        DEBUG_NONE = 0,
        DEBUG_INDIRECT = 1,     // the indirect light alone, over the opaque pixels
        DEBUG_LIGHT = 2,        // the light the samples read (the last frame's, moved)
        DEBUG_MISSES = 3,       // the same, red where it was not found (off the last screen, or hidden)
        DEBUG_NOT_ADDED = 4,    // every pass, nothing added to the lighting
    };
    constexpr U32 DEBUG_LAST = DEBUG_NOT_ADDED;

    // 1 at most: each bounce gives back less than it took (research/164 4.1,
    // decided 2026-10-25), so the light that feeds on itself settles.
    constexpr F32 MAX_STRENGTH = 1.f;
    constexpr F32 MIN_RADIUS = 0.5f;            // m (PlusSsgiRadius)
    constexpr F32 MAX_RADIUS = 5.f;
    constexpr F32 MIN_THICKNESS = 0.1f;         // m (PlusSsgiThickness)
    constexpr F32 MAX_THICKNESS = 2.f;

    // The captured light's ceiling (its brightest channel, hue kept). Under the
    // "no post" tone mapping (plus::isNoPost) the picture is clamped to 1: no
    // light above 1 shows anyway; a classic sky (plus::isClassicSky, its
    // colours taken as they are) is held there too. Else this many times the
    // light of the sun (or moon) as the scene takes it, and at least 1, so
    // lamps at night still count: full bright signs and glowing faces do not
    // dye everything near.
    constexpr F32 NO_POST_LIGHT_CAP = 1.f;
    constexpr F32 SUNLIGHT_CAP_SCALE = 2.f;
    constexpr F32 MIN_LIGHT_CAP = 1.f;

    // A point whose depth a frame ago differs by more than this share from the
    // capture's was hidden then: it gets no light (research/164 3.2).
    constexpr F32 DISOCCLUSION = 0.05f;
    // The light fades out over this share of the last frame's screen at its edges.
    constexpr F32 EDGE_FADE = 0.05f;
    // A camera that moved further than this in a frame jumped (a teleport, a
    // region crossing): the last frame's light is not used.
    constexpr F32 JUMP_METRES = 10.f;
    // A snapshot is drawn in frames of its own (tiles, another size) without
    // capturing; it reads the main view's capture from up to this many frames before.
    constexpr U32 SNAPSHOT_MAX_AGE = 64;

    F32 settingIn(F32 value, F32 low, F32 high, F32 fallback)
    {
        return llfinite(value) ? llclamp(value, low, high) : fallback;
    }

    F32 strength()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_STRENGTH, 1.f);
        return settingIn(setting, 0.f, MAX_STRENGTH, 1.f);
    }

    U32 debugMode()
    {
        static LLCachedControl<U32> setting(gSavedSettings, SETTING_DEBUG, DEBUG_NONE);
        return setting <= DEBUG_LAST ? (U32)setting : DEBUG_NONE;
    }

    bool backendReady()
    {
        return LLRHI::sSsgi && LLRHI::sSsgiCapture && LLRHI::sSsgiReproject && LLRHI::sSsgiOcclusion && LLRHI::sSsgiDenoise &&
               LLRHI::sSsgiDebugView && LLRHI::sSsgiCompactLight;
    }

    // What SSGI needs besides its own settings (plus::ssgi::unavailableReason()),
    // the first missing; nullptr when nothing is. In the order the user would
    // fix them: what no setting changes first (the backend), then GTAO's own
    // (the official SSAO), then the settings SSGI builds on. enabled() runs it
    // every frame: no lookup by name but the feature table's, once a session.
    const char* blocker()
    {
        namespace reason = plus::gtao::reason;
        if (!LLRHI::sSsgi)
        {
            return plus::safemode::active() ? reason::SAFE_MODE : reason::SSGI_SWITCHED_OFF;
        }
        if (!backendReady())
        {
            return reason::NO_BACKEND;
        }
        if (const char* gtao = plus::gtao::unavailableReason())
        {
            return gtao;
        }
        static LLCachedControl<bool> gtao_on(gSavedSettings, "PlusGtao", false);
        if (!gtao_on)
        {
            return reason::GTAO_OFF;
        }
        // GTAO draws nothing at strength 0 (plus/gtao readSettings()), and SSGI's passes are in its
        static LLCachedControl<F32> gtao_strength(gSavedSettings, "PlusGtaoStrength", 1.f);
        if (!((F32)gtao_strength > 0.f))
        {
            return reason::GTAO_STRENGTH_ZERO;
        }
        static LLCachedControl<bool> hdr(gSavedSettings, "RenderHDREnabled", true);
        if (!hdr)
        {
            return reason::HDR_OFF;
        }
        if (!LLPipeline::sReflectionProbesEnabled)
        {
            // the feature table is set at start: looked up by name once a session
            static const bool in_table = LLFeatureManager::getInstance()->isFeatureAvailable("RenderReflectionsEnabled");
            return in_table ? reason::PROBES_OFF : reason::PROBES_UNSUPPORTED;
        }
        return nullptr;
    }

    // The setting and what it needs, whatever is drawn now.
    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ENABLED, false);
        return on && strength() > 0.f && !blocker();
    }

    bool mainView()
    {
        return !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT;
    }

    // ------------------------------------------------------------------
    // The targets. Made on first use and kept while the feature is on; never
    // destroyed by a static destructor, which would run without a GL context.

    struct Targets
    {
        LLRenderTarget light;           // RGBA16F, half the screen: the last frame's light (rgb) and depth (a)
        LLRenderTarget reprojected;     // R11F_G11F_B10F (or RGBA16F), GTAO's grid: that light moved into this frame
        LLRenderTarget occlusion;       // RG16F visibility and depth, RGBA16F indirect light and depth
        LLRenderTarget denoised;        // the same, averaged: GtaoApply reads 0, the lighting 1
        // After one could not be made (VRAM full): not again every frame.
        plus::RetryWait retry;          // reprojected, occlusion, denoised
        plus::RetryWait light_retry;    // light
    };

    Targets& targets()
    {
        static Targets* targets = new Targets();
        return *targets;
    }

    struct State
    {
        U32 light_frame = 0;            // gFrameCount of the capture in Targets::light (0: none)
        glm::mat4 light_projection = glm::mat4(1.f);    // its projection
        bool live = false;              // denoised holds this frame's indirect light
        U32 live_frame = 0;             // gFrameCount it was drawn in
        U32 full_width = 0;             // the screen it was drawn for
        U32 full_height = 0;
        U32 scale = 1;                  // its grid: full-resolution pixels per pixel, each way
        plus::RetryWait failure;        // after a pass could not be drawn: GTAO alone for a while
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

    void release()
    {
        Targets& t = targets();
        releaseTarget(t.light);
        releaseTarget(t.reprojected);
        releaseTarget(t.occlusion);
        releaseTarget(t.denoised);
        State& s = state();
        s.light_frame = 0;
        s.live = false;
    }

    // Point sampling (the passes read texels as they are, texelFetch) on
    // each attachment; then unit 0 is left empty, not holding a new texture
    // in the middle of the lighting.
    void pointSampled(LLRenderTarget& target)
    {
        for (U32 i = 0; i < target.getNumTextures(); ++i)
        {
            target.bindTexture(i, 0, LLTexUnit::TFO_POINT);
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    // target at width x height in format (and second, a second attachment, unless 0).
    bool sizeTarget(LLRenderTarget& target, U32 width, U32 height, U32 format, U32 second, plus::RetryWait& retry)
    {
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        if (!retry.due())
        {
            return false;       // one could not be made a moment ago: not again every frame
        }
        if (!target.allocate(width, height, format) || (second && !target.addColorAttachment(second)))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            retry.failed();
            LL_WARNS_ONCE("PlusSsgi") << "SSGI: no target of " << width << "x" << height << LL_ENDL;
            return false;
        }
        retry.succeeded();
        pointSampled(target);
        return true;
    }

    // The format of the reprojected light: 32 bits a pixel where the GPU can draw into it.
    U32 reprojectedFormat()
    {
        return LLRHI::sSsgiCompactLight() ? GL_R11F_G11F_B10F : GL_RGBA16F;
    }

    LLRHI::GtaoView viewOf(const glm::mat4& projection, const LLRenderTarget& gbuffer)
    {
        LLRHI::GtaoView view;
        view.depth_a = projection[2][2];
        view.depth_b = projection[3][2];
        view.projection[0] = projection[0][0];
        view.projection[1] = projection[1][1];
        view.projection[2] = projection[2][0];
        view.projection[3] = projection[2][1];
        view.depth_texture = gbuffer.getDepth();
        view.width = gbuffer.getWidth();
        view.height = gbuffer.getHeight();
        return view;
    }

    // The captured light's ceiling (see NO_POST_LIGHT_CAP).
    F32 lightCap()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        // A classic sky takes its colours as they are (sRGB): held at 1 as under "no post" (research/164 4.1).
        if (plus::isNoPost(sky.get()) || plus::isClassicSky(sky.get()))
        {
            return NO_POST_LIGHT_CAP;
        }
        // The light as softenLightF.glsl takes it (made linear).
        const LLColor4& diffuse = LLEnvironment::instance().getIsSunUp() ? gPipeline.mSunDiffuse : gPipeline.mMoonDiffuse;
        const LLColor3 light = linearColor3(diffuse);
        const F32 brightest = llmax(light.mV[VRED], light.mV[VGREEN], light.mV[VBLUE]);
        return llfinite(brightest) ? llmax(SUNLIGHT_CAP_SCALE * brightest, MIN_LIGHT_CAP) : MIN_LIGHT_CAP;
    }

    // Whether the capture is the light of the frame just before this one (a
    // snapshot: of the main view just before it) and the camera did not jump.
    bool lightUsable()
    {
        const State& s = state();
        const Targets& t = targets();
        if (!s.light_frame || !t.light.isComplete())
        {
            return false;
        }
        const bool recent = gSnapshot ? gFrameCount - s.light_frame <= SNAPSHOT_MAX_AGE : s.light_frame + 1 == gFrameCount;
        if (!recent)
        {
            return false;
        }
        const glm::vec3 moved(gGLDeltaModelView[3]);
        return glm::length(moved) <= JUMP_METRES;
    }

    // The lit opaque pixels at half size, for the next frame.
    void captureLight(const LLRHI::GtaoView& view, const glm::mat4& projection)
    {
        State& s = state();
        Targets& t = targets();
        s.light_frame = 0;
        const U32 width = (view.width + 1) / 2;
        const U32 height = (view.height + 1) / 2;
        if (!sizeTarget(t.light, width, height, GL_RGBA16F, 0, t.light_retry))
        {
            return;
        }
        LLRHI::SsgiCapture pass;
        pass.view = view;
        pass.color_texture = gPipeline.mRT->screen.getTexture();
        pass.width = width;
        pass.height = height;
        pass.light_cap = lightCap();
        t.light.bindTarget();
        const bool drawn = LLRHI::sSsgiCapture(pass);
        t.light.flush();
        if (drawn)
        {
            s.light_frame = gFrameCount;
            s.light_projection = projection;
        }
    }

    // PlusSsgiDebug 1-3: over the bound lit screen.
    void drawDebugView(const LLRHI::GtaoView& view, U32 debug)
    {
        const State& s = state();
        Targets& t = targets();
        if (!s.live || s.live_frame != gFrameCount || s.full_width != view.width || s.full_height != view.height)
        {
            return;
        }
        LLRenderTarget& source = debug == DEBUG_INDIRECT ? t.denoised : t.reprojected;
        LLRHI::SsgiDebugView pass;
        pass.view = view;
        pass.source_texture = source.getTexture(debug == DEBUG_INDIRECT ? 1 : 0);
        pass.width = source.getWidth();
        pass.height = source.getHeight();
        pass.scale = s.scale;
        LLRHI::sSsgiDebugView(pass);
    }

    // Settings: app_settings/settings_plus.xml (PlusSsgi*), set in the photo
    // tools (app_settings/plus/phototools/catalog.xml, tab "color": Light & Color).
    // Drawn from plus/gtao and the <plus> hooks in LLPipeline::renderDeferredLighting()
    // and bindReflectionProbes(); softenLightF.glsl adds it (its <plus> lines).
    plus::FeatureRegistration sRegistration({ "ssgi", nullptr, {} });
}

namespace plus::ssgi
{
    const char* unavailableReason()
    {
        return blocker();
    }

    bool available()
    {
        return !blocker();
    }

    bool wanted()
    {
        return enabled() && mainView() && state().failure.due();
    }

    bool sizeTargets(U32 width, U32 height)
    {
        Targets& t = targets();
        return sizeTarget(t.reprojected, width, height, reprojectedFormat(), 0, t.retry) &&
               sizeTarget(t.occlusion, width, height, GL_RG16F, GL_RGBA16F, t.retry) &&
               sizeTarget(t.denoised, width, height, GL_RG16F, GL_RGBA16F, t.retry);
    }

    U32 renderOcclusion(const LLRHI::GtaoOcclusion& occlusion)
    {
        static LLCachedControl<F32> radius(gSavedSettings, SETTING_RADIUS, 2.f);
        static LLCachedControl<F32> thickness(gSavedSettings, SETTING_THICKNESS, 0.5f);
        State& s = state();
        Targets& t = targets();
        s.live = false;
        const U32 width = occlusion.width;
        const U32 height = occlusion.height;
        if (!sizeTargets(width, height))
        {
            s.failure.failed();     // GTAO's own targets from the next frame
            return 0;
        }
        LLRHI::gpuMark("ssgi");

        // The last frame's light, where each point was then.
        LLRHI::SsgiReproject reproject;
        reproject.view = occlusion.view;
        reproject.view_depth_texture = occlusion.view_depth_texture;
        reproject.width = width;
        reproject.height = height;
        reproject.scale = occlusion.scale;
        if (lightUsable())
        {
            reproject.light_texture = t.light.getTexture();
            reproject.light_width = t.light.getWidth();
            reproject.light_height = t.light.getHeight();
            // This frame's eye space -> the last frame's (renderGeomDeferred set it for the main camera) -> its clip space.
            const glm::mat4 to_light = s.light_projection * gGLInverseDeltaModelView;
            memcpy(reproject.reproject, glm::value_ptr(to_light), sizeof(reproject.reproject));
        }
        reproject.disocclusion = DISOCCLUSION;
        reproject.edge_fade = EDGE_FADE;
        reproject.show_misses = debugMode() == DEBUG_MISSES;
        t.reprojected.bindTarget();
        bool drawn = LLRHI::sSsgiReproject(reproject);
        t.reprojected.flush();

        if (drawn)
        {
            LLRHI::SsgiOcclusion pass;
            pass.occlusion = occlusion;
            pass.light_texture = t.reprojected.getTexture();
            pass.light_radius = settingIn(radius, MIN_RADIUS, MAX_RADIUS, 2.f);
            pass.thickness = settingIn(thickness, MIN_THICKNESS, MAX_THICKNESS, 0.5f);
            t.occlusion.bindTarget();
            drawn = LLRHI::sSsgiOcclusion(pass);
            t.occlusion.flush();
        }

        if (drawn)
        {
            LLRHI::SsgiDenoise denoise;
            denoise.occlusion_texture = t.occlusion.getTexture(0);
            denoise.indirect_texture = t.occlusion.getTexture(1);
            denoise.width = width;
            denoise.height = height;
            t.denoised.bindTarget();
            drawn = LLRHI::sSsgiDenoise(denoise);
            t.denoised.flush();
        }

        LLRHI::gpuMark("gtao");     // GTAO's apply pass comes next
        if (!drawn)
        {
            s.failure.failed();
            LL_WARNS("PlusSsgi") << "SSGI could not be drawn; GTAO alone for " << plus::RetryWait::RETRY_SECONDS << " s" << LL_ENDL;
            return 0;
        }
        s.failure.succeeded();
        s.live = true;
        s.live_frame = gFrameCount;
        s.full_width = occlusion.view.width;
        s.full_height = occlusion.view.height;
        s.scale = llmax(1u, occlusion.scale);
        static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
        const LLError::ELevel log_level = resize_log.level(width, height, s.scale);  // once: LL_VLOGS reads its level twice
        LL_VLOGS(log_level, "PlusSsgi") << "SSGI: " << width << "x" << height << " for a " << occlusion.view.width << "x"
                                        << occlusion.view.height << " screen" << LL_ENDL;
        return t.denoised.getTexture(0);
    }

    void capture()
    {
        if (!enabled())
        {
            release();
            return;
        }
        if (!mainView() || LLRenderTarget::getCurrentBoundTarget() != &gPipeline.mMainRT.screen)
        {
            return;
        }
        const LLRenderTarget& screen = gPipeline.mRT->screen;
        const LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (!screen.isComplete() || !gbuffer.getDepth() || screen.getWidth() != gbuffer.getWidth() ||
            screen.getHeight() != gbuffer.getHeight())
        {
            state().light_frame = 0;
            return;
        }
        const glm::mat4 projection = get_current_projection();
        const LLRHI::GtaoView view = viewOf(projection, gbuffer);
        const U32 debug = debugMode();
        gGL.flush();
        LLRHI::gpuMark("ssgi");
        // A snapshot's tiles keep the main view's light (they are parts of its view).
        if (!gSnapshot)
        {
            captureLight(view, projection);
        }
        // After the capture: the next frame's light is the scene, not the view.
        if (debug >= DEBUG_INDIRECT && debug <= DEBUG_MISSES)
        {
            drawDebugView(view, debug);
        }
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest goes back to its segment
    }

    void bind(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_map("plusSsgiMap");
        static LLStaticHashedString s_params("plusSsgiParams");
        static LLStaticHashedString s_size("plusSsgiSize");
        if (shader.getUniformLocation(s_params) < 0)
        {
            return;     // not the lighting program (or one without the official SSAO)
        }
        const State& s = state();
        Targets& t = targets();
        // plusSsgiMap is not a reserved uniform: linking gave it no unit. It
        // takes the first after the program's own (plus/avatarrefl and its
        // water mirror keep the last four).
        const S32 unit = shader.mActiveTextureChannels;
        const LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        const bool live = s.live && s.live_frame == gFrameCount && &shader == &gDeferredSoftenProgram && mainView() &&
                          t.denoised.isComplete() && s.full_width == gbuffer.getWidth() && s.full_height == gbuffer.getHeight() &&
                          debugMode() != DEBUG_NOT_ADDED && unit >= 0 && unit < (S32)LL_NUM_TEXTURE_LAYERS - 4;
        if (!live)
        {
            // Nothing added; the sampler points at a texture the program has bound (never read).
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
            shader.uniform1i(s_map, llmax(shader.getTextureChannel(LLShaderMgr::DEFERRED_DIFFUSE), 0));
            return;
        }
        shader.uniform1i(s_map, unit);
        gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, t.denoised.getTexture(1));
        gGL.getTexUnit(0)->activate();
        shader.uniform4f(s_params, strength(), (F32)s.scale, 0.f, 0.f);
        shader.uniform4f(s_size, (F32)s.full_width, (F32)s.full_height, (F32)t.denoised.getWidth(), (F32)t.denoised.getHeight());
    }
}
