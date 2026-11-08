/**
 * @file watermirror.cpp
 * @brief The water as a mirror (research/139). Part of plus/avatarrefl.
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

// Off by default (PlusWaterMirror 0; Vulkan only).
//
// Before PBR the viewer drew the world again from the camera mirrored in the
// water and laid it on the water; since then the water reflects the probes,
// the screen space reflections and the mirror probe only. This brings the
// mirrored pass back, as the avatar reflection (avatarrefl.cpp) draws its own:
// before the main view, the eye is mirrored in the region's water plane and
// the world is drawn from there with an ordinary right-handed camera (the
// mirrored at and up axes) through the official deferred renderer, as for a
// reflection probe face (gCubeSnapshot), into a target pack of our own at a
// share of the screen's size. A pixel of the water and its reflection lie on
// the same ray from the mirrored eye, so the water shader
// (reflectionProbeF.glsl, sampleReflectionProbesWater) projects the pixel's
// position with that camera and reads the picture there, moved a little by
// the waves' normal as the old water did.
//
// What is drawn (PlusWaterMirror): 1 the sky and the terrain, 2 also the
// trees and the grass, 3 also the objects, 4 also the avatars (and the
// particles). The render types of the rest are cleared, so their partitions
// are not even culled. The cull keeps what is above the water (updateCull's
// water clip, as for a probe face: the mirrored eye is under the water, the
// pass is culled as from above it) and the fragment shaders discard the rest
// (the mirrors' clip plane, through the <plus> lines in
// LLSettingsVOWater::applySpecial()): what is under the water would hide the
// reflection from the mirrored eye.
//
// Costs kept down: the far clip (PlusWaterMirrorDistance, for the plants, the
// objects and the avatars; the terrain has no far clip), the main view's LOD
// as it is (a probe face's rule: nothing is rebuilt for the pass), the
// batches outside the view left out (sProbeFaceRender, as a probe face), no
// sun shadows unless PlusWaterMirrorShadows, a picture every
// PlusWaterMirrorRate frames (read with the camera it was drawn with, so it
// stays in place while the view turns), and no pass at all while the region's
// water was not drawn in the last frame or the camera is under it (the
// targets are let go after some 10 s without one, and at once when it is off).
//
// The picture's edges (PlusWaterMirrorAA): the pass has no anti-aliasing of
// its own (a G-buffer of a probe face's rules), so the water smooths them
// where it reads the picture: FXAA's edge search on the picture's texels, or
// four taps on a rotated grid; no pass or target is added. Above the screen's
// size (PlusWaterMirrorScale over 1) every choice averages the screen pixel's
// texels, the supersampling's resolve. Without the mirror's shadows the
// pack's sun maps are made small (they are only cleared).

#include "llviewerprecompiledheaders.h"

#include "watermirror.h"
#include "avatarrefl.h"

#include "skinsss/skinsss.h"
#include "plus/shaderreload/shaderreload.h"

#include "llagentcamera.h"
#include "llappviewer.h"
#include "lldrawpool.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llscenerecords.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "pipeline.h"
#include "plus.h"

#include <glm/gtc/type_ptr.hpp>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_MODE[]     = "PlusWaterMirror";
    constexpr char SETTING_SCALE[]    = "PlusWaterMirrorScale";
    constexpr char SETTING_RATE[]     = "PlusWaterMirrorRate";
    constexpr char SETTING_DISTANCE[] = "PlusWaterMirrorDistance";
    constexpr char SETTING_SHADOWS[]  = "PlusWaterMirrorShadows";
    constexpr char SETTING_WAVES[]    = "PlusWaterMirrorWaves";
    constexpr char SETTING_AA[]       = "PlusWaterMirrorAA";

    // PlusWaterMirror: how much of the world is mirrored.
    enum Mode : U32
    {
        MODE_OFF = 0,
        MODE_SKY_TERRAIN = 1,
        MODE_PLANTS = 2,
        MODE_OBJECTS = 3,
        MODE_AVATARS = 4,
    };

    // The camera must be at least this far above the water.
    constexpr F32 MIN_EYE_ABOVE_WATER = 0.05f;
    // The fragments are kept down to this far under the water (m), so the
    // terrain meets the water's edge in the picture without a seam.
    constexpr F32 CLIP_BELOW_WATER = 0.02f;

    // The far clip (PlusWaterMirrorDistance) stays within these and the draw distance.
    constexpr F32 MIN_DISTANCE = 16.f;
    constexpr F32 DEFAULT_DISTANCE = 128.f;

    // PlusWaterMirrorRate: a picture every 1 to MAX_RATE frames. Between them
    // a new one is drawn anyway when the camera moved or turned this much (a
    // turn shows water the old picture does not cover).
    constexpr U32 MAX_RATE = 4;
    constexpr F32 REDRAW_MOVE = 1.f;            // m
    constexpr F32 REDRAW_TURN_COS = 0.985f;     // about 10 degrees

    // The mirror fades in over this long when it starts (onto the water, out of it).
    constexpr F32 FADE_SECONDS = 0.25f;

    // The waves' strength in the shader: PlusWaterMirrorWaves x this x the
    // water's "reflection wavelet scale" above the water (getScaleAbove(), the
    // old water's distortion). Moves the picture about as far as the old water
    // did at the default water settings (PLAUSIBLE, see research/139).
    constexpr F32 WAVE_GAIN = 30.f;
    constexpr F32 MAX_WAVES = 4.f;

    // The texture units of the two samplers, not among the official reserved
    // uniforms: the two under the avatar reflection's (the last two). Other
    // plus samplers outside the reserved list take units from
    // mActiveTextureChannels up (plus/ocean takes that one), so a program
    // whose own units reach MAP_UNIT goes without the mirror.
    constexpr S32 MAP_UNIT = (S32)LL_NUM_TEXTURE_LAYERS - 4;
    constexpr S32 DEPTH_UNIT = (S32)LL_NUM_TEXTURE_LAYERS - 3;

    // PlusWaterMirrorScale: the picture's share of the screen each way. Above
    // 1 it is supersampled: the water reads the screen pixel's texels
    // together (reflectionProbeF.glsl, plusWaterMirrorColor).
    constexpr F32 MIN_SCALE = 0.5f;
    constexpr F32 MAX_SCALE = 2.f;
    constexpr F32 DEFAULT_SCALE = 0.5f;
    constexpr U32 MIN_TARGET_SIZE = 64;
    // Neither side beyond this (a 4K screen at 2x would be 7680 wide: some
    // 1.2 GB of G-buffer); the shape is kept.
    constexpr U32 MAX_TARGET_SIZE = 4096;
    // The sun's maps of the pack while the mirror draws no shadows: only
    // cleared to "all lit" and read so, at any size (a screen's share would
    // be 4 maps of the picture's size for nothing).
    constexpr U32 BLANK_SHADOW_MAP_SIZE = 64;

    // PlusWaterMirrorAA: how the water reads the picture (reflectionProbeF.glsl,
    // plusWaterMirrorColor and plusWaterMirrorGeom).
    enum AntiAlias : U32
    {
        AA_NONE = 0,        // one bilinear tap (as before)
        AA_EDGES = 1,       // FXAA's edge smoothing on the picture
        AA_SOFT = 2,        // four taps on a rotated grid
    };

    // The region's water must have been drawn in the main view this recently (frames).
    constexpr U32 WATER_SEEN_FRAMES = 2;
    // The targets are released after this many frames without a pass (some 10 s).
    constexpr U32 IDLE_RELEASE_FRAMES = 600;

    struct State
    {
        bool shader_define = false;     // the programs were built with PLUS_WATER_MIRROR
        bool valid = false;             // the target holds the world mirrored in the water, drawn with view_proj
        bool drawing = false;           // the mirror's pass is being drawn (clipAtWater)
        bool shadow_maps_blank = false; // the pack's sun maps were cleared and not drawn since
        U32 mode = MODE_OFF;            // what the picture holds
        F32 water_height = 0.f;         // the plane it was mirrored in (agent space)
        glm::mat4 view_proj{ 1.f };     // the mirrored camera's projection * view
        F32 fade = 0.f;                 // 0..1, the mirror's weight
        U32 frames_since_draw = 0;
        U32 idle_frames = 0;            // frames in a row the pass could not run
        LLVector3 drawn_origin;         // the camera (not mirrored) the picture was drawn for
        LLVector3 drawn_at;
        U32 water_frame = 0;            // the last frame the main view drew the region's water
        bool water_frame_set = false;
        U32 screen_generation = 0;      // plus::skinsss::screenBuffersGeneration() the target was made at
        bool shadow_maps_small = false; // the pack was made for a pass without shadows (its sun maps small when they could be)
        plus::RetryWait retry;          // after the targets could not be made (VRAM full)
    };

    State& state()
    {
        static State s;
        return s;
    }

    // Made on first use and kept while the mirror is on; never destroyed by a
    // static destructor, which would run without a GL context.
    LLPipeline::RenderTargetPack& targets()
    {
        static LLPipeline::RenderTargetPack* pack = new LLPipeline::RenderTargetPack();
        return *pack;
    }

    // The samplers' units hold the 1x1 white texture: never the pack while it
    // is drawn into or released, whichever program reads the units then.
    void blankUnits()
    {
        if (LLViewerTexture* blank = LLViewerFetchedTexture::sWhiteImagep.get())
        {
            gGL.getTexUnit(MAP_UNIT)->bind(blank);
            gGL.getTexUnit(DEPTH_UNIT)->bind(blank);
            gGL.getTexUnit(0)->activate();
        }
    }

    // Also a half-made one (an allocation that failed is not isComplete() but
    // may hold its framebuffer); release() of an empty target does nothing.
    void releaseTarget(LLRenderTarget& target)
    {
        target.release();
    }

    void releaseTargets()
    {
        blankUnits();
        LLPipeline::RenderTargetPack& pack = targets();
        releaseTarget(pack.screen);
        releaseTarget(pack.deferredScreen);
        releaseTarget(pack.deferredLight);
        for (LLRenderTarget& shadow : pack.shadow)
        {
            releaseTarget(shadow);
        }
        pack.width = 0;
        pack.height = 0;
        State& s = state();
        s.valid = false;
        s.shadow_maps_blank = false;
        s.shadow_maps_small = false;
    }

    // The pack at width x height, made as a probe's or a mirror's
    // (allocateScreenBufferInternal with gCubeSnapshot set: the G-buffer, the
    // lit screen and the sun's maps, nothing of the main view's). Made again
    // when the main view's are (a resize, the shadow or HDR settings), when
    // PlusWaterMirrorScale changes, and when the mirror's shadows are turned
    // on or off (the sun's maps are small without them).
    bool sizeTargets(U32 width, U32 height, bool shadows)
    {
        State& s = state();
        LLPipeline::RenderTargetPack& pack = targets();
        const U32 generation = plus::skinsss::screenBuffersGeneration();
        if (pack.screen.isComplete() && pack.width == width && pack.height == height && s.screen_generation == generation &&
            s.shadow_maps_small == !shadows)
        {
            return true;
        }
        if (!s.retry.due())
        {
            return false;   // a G-buffer of the screen's share could not be made a moment ago: not again every frame
        }
        releaseTargets();

        LLPipeline::RenderTargetPack* const main_rt = gPipeline.mRT;
        gPipeline.mRT = &pack;
        gCubeSnapshot = true;
        const bool made = gPipeline.allocateScreenBufferInternal(width, height);
        // Without shadows the pass only clears the sun's maps (blankShadowMaps):
        // made again small, through the same path (their filtering and compare modes).
        const bool small_shadows = made && !shadows && pack.shadow[0].isComplete();
        const bool shadows_made = !small_shadows || gPipeline.allocateShadowBuffer(BLANK_SHADOW_MAP_SIZE, BLANK_SHADOW_MAP_SIZE);
        gCubeSnapshot = false;
        gPipeline.mRT = main_rt;

        if (!made || !shadows_made || !pack.screen.isComplete() || !pack.deferredScreen.isComplete())
        {
            LL_WARNS_ONCE("Plus") << "Water mirror: no targets of " << width << "x" << height << LL_ENDL;
            releaseTargets();
            s.retry.failed();
            return false;
        }
        s.retry.succeeded();
        s.screen_generation = generation;
        // The key follows the setting (also with no sun maps at all, RenderShadowDetail 0).
        s.shadow_maps_small = !shadows;
        static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
        const LLError::ELevel log_level = resize_log.level(pack.deferredScreen.getWidth(), pack.deferredScreen.getHeight(), small_shadows ? 1 : 0);    // once: LL_VLOGS reads its level twice
        LL_VLOGS(log_level, "Plus")
            << "Water mirror targets " << pack.deferredScreen.getWidth() << "x" << pack.deferredScreen.getHeight()
                         << (small_shadows ? ", sun maps blank" : "") << LL_ENDL;
        return true;
    }

    U32 modeSetting()
    {
        static LLCachedControl<U32> mode(gSavedSettings, SETTING_MODE, MODE_OFF);
        return llmin((U32)mode, (U32)MODE_AVATARS);
    }

    // Whether the programs should have PLUS_WATER_MIRROR for this mode.
    bool wantsShaderDefine(U32 mode)
    {
        return mode != MODE_OFF && LLRHI::sVulkanGL;
    }

    void setShaders()
    {
        LLViewerShaderMgr::instance()->setShaders();
    }

    // Turned on or off: the programs get or lose the define (made again, as
    // for the other settings that change shaders: plus/shaderreload makes them
    // in the background or on several threads). Between modes 1 to 4 nothing
    // is made again. While a reload is on its way the programs drawn with are
    // not those it makes: asked for again (merged).
    void onModeChanged(U32 mode)
    {
        if (wantsShaderDefine(llmin(mode, (U32)MODE_AVATARS)) == state().shader_define && !plus::shaderreload::pending())
        {
            return;
        }
        setShaders();
    }

    LLVector3 mirroredInWater(const LLVector3& v)
    {
        return LLVector3(v.mV[VX], v.mV[VY], -v.mV[VZ]);
    }

    // The picture's size: PlusWaterMirrorScale of the main view's each way,
    // neither side beyond MAX_TARGET_SIZE (the shape kept) nor under MIN_TARGET_SIZE.
    void targetSize(U32& width, U32& height)
    {
        static LLCachedControl<F32> scale_setting(gSavedSettings, SETTING_SCALE, DEFAULT_SCALE);
        const F32 scale_value = scale_setting;   // NaN would pass llclamp() and make (U32)NaN below
        F32 scale = llclamp(std::isfinite(scale_value) ? scale_value : DEFAULT_SCALE, MIN_SCALE, MAX_SCALE);
        const U32 main_side = llmax(llmax(gPipeline.mMainRT.width, gPipeline.mMainRT.height), 1u);
        scale = llmin(scale, (F32)MAX_TARGET_SIZE / (F32)main_side);
        width = llclamp((U32)(gPipeline.mMainRT.width * scale + 0.5f), MIN_TARGET_SIZE, MAX_TARGET_SIZE);
        height = llclamp((U32)(gPipeline.mMainRT.height * scale + 0.5f), MIN_TARGET_SIZE, MAX_TARGET_SIZE);
    }

    // ------------------------------------------------------------------
    // When to draw

    // Whether the pass may run in this frame at all.
    bool canDraw()
    {
        return LLRHI::sVulkanGL && state().shader_define && LLPipeline::sReflectionProbesEnabled && !gCubeSnapshot &&
               !gTeleportDisplay && !gDisconnected && LLStartUp::getStartupState() >= STATE_STARTED &&
               !LLAppViewer::instance()->logoutRequestSent() &&
               // Under the water there is no mirror (and the water program is the other one).
               !LLViewerCamera::getInstance()->cameraUnderWater();
    }

    // The water the cull clips at (LLPipeline::setCullClipPlane) and
    // applySpecial's water plane, when the camera is far enough above it.
    bool waterUnderEye(F32& water_height)
    {
        water_height = LLEnvironment::instance().getWaterHeight();
        return LLViewerCamera::getInstance()->getOrigin().mV[VZ] - water_height >= MIN_EYE_ABOVE_WATER;
    }

    // Whether the main view drew the region's water lately (bind() notes it).
    bool waterSeen()
    {
        const State& s = state();
        return s.water_frame_set && LLFrameTimer::getFrameCount() - s.water_frame <= WATER_SEEN_FRAMES;
    }

    // Whether the picture must be drawn again in this frame.
    bool due(U32 mode, F32 water_height)
    {
        static LLCachedControl<U32> rate_setting(gSavedSettings, SETTING_RATE, 1);
        State& s = state();
        const U32 rate = llclamp((U32)rate_setting, 1u, MAX_RATE);
        ++s.frames_since_draw;
        if (!s.valid || s.mode != mode || s.water_height != water_height || s.frames_since_draw >= rate)
        {
            return true;
        }
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        return (camera->getOrigin() - s.drawn_origin).lengthSquared() > REDRAW_MOVE * REDRAW_MOVE ||
               camera->getAtAxis() * s.drawn_at < REDRAW_TURN_COS;
    }

    // ------------------------------------------------------------------
    // The pass

    // The mirrored camera's far clip: PlusWaterMirrorDistance, within the draw distance.
    F32 farClip()
    {
        static LLCachedControl<F32> distance(gSavedSettings, SETTING_DISTANCE, DEFAULT_DISTANCE);
        const F32 draw_distance = llmax(gAgentCamera.mDrawDistance, MIN_DISTANCE);
        const F32 distance_value = distance;   // NaN would pass llclamp() into setFar()
        return llclamp(std::isfinite(distance_value) ? distance_value : DEFAULT_DISTANCE, MIN_DISTANCE, draw_distance);
    }

    // The sun's maps as nothing casting a shadow (all lit): the pass without
    // shadows reads them so. Cleared once, and again after a pass with shadows.
    void blankShadowMaps(LLPipeline::RenderTargetPack& pack)
    {
        LLGLDepthTest depth(GL_TRUE);
        for (LLRenderTarget& shadow : pack.shadow)
        {
            if (shadow.isComplete())
            {
                shadow.bindTarget();
                shadow.clear();
                shadow.flush();
            }
        }
    }

    // display_cube_face() (llviewerdisplay.cpp) for a target of our own size,
    // as avatarrefl.cpp's drawFace(): the camera is set up here rather than by
    // display_update_camera(), which takes the main view's rectangle, and the
    // mirrored eye (under the water) is culled as from above it.
    void drawFace(LLViewerCamera& camera, LLPipeline::RenderTargetPack& pack, bool shadows)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("water mirror face");

        State& s = state();
        LLGLSDefault gls_default;
        LLGLDepthTest gls_depth(GL_TRUE, GL_TRUE, GL_LEQUAL);

        LLVertexBuffer::unbind();
        gPipeline.disableLights();
        gPipeline.mBackfaceCull = true;

        // The camera's near clip and MAX_FAR_CLIP, as display_update_camera()
        // gives a probe face (the terrain and the sky reach past the far clip).
        const S32 width = pack.deferredScreen.getWidth();
        const S32 height = pack.deferredScreen.getHeight();
        camera.setPerspective(NOT_FOR_SELECTION, 0, 0, width, height, false);
        s.view_proj = get_current_projection() * get_current_modelview();

        // The sky's and water's uniforms for this camera, the clip at the
        // water among them (clipAtWater, while s.drawing).
        LLEnvironment::instance().update(&camera);

        LLSpatialGroup::sNoDelete = true;

        static LLCullResult result;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        LLPipeline::sUnderWaterRender = false;
        gPipeline.updateCull(camera, result);

        gGL.setColorMask(true, true);
        LLRHI::clearColor(0.f, 0.f, 0.f, 0.f);
        if (shadows)
        {
            gPipeline.generateSunShadow(camera);
            s.shadow_maps_blank = false;
        }
        else if (!s.shadow_maps_blank)
        {
            blankShadowMaps(pack);
            s.shadow_maps_blank = true;
        }
        // generateSunShadow leaves the last shadow camera current; sort for the world camera
        // (as display() does), so setVisible marks this view's slot and postSort leaves out
        // the batches outside the view
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

        gPipeline.stateSort(camera, result);

        gGL.setColorMask(true, true);
        pack.deferredScreen.bindTarget();
        LLRHI::clearColor(0.f, 0.f, 0.f, 0.f);
        pack.deferredScreen.clear();

        gPipeline.renderGeomDeferred(camera);

        pack.deferredScreen.flush();

        gPipeline.renderDeferredLighting();

        LLSpatialGroup::sNoDelete = false;
        gPipeline.clearReferences();
    }

    // What the mode leaves out of the world, besides what never belongs in
    // the mirror (the water itself, the HUD, what is under the water).
    void maskForMode(U32 mode)
    {
        gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_WATER, LLPipeline::RENDER_TYPE_VOIDWATER,
                                      LLPipeline::RENDER_TYPE_WATEREXCLUSION, LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER,
                                      LLPipeline::RENDER_TYPE_HUD, LLPipeline::RENDER_TYPE_HUD_PARTICLES, LLPipeline::END_RENDER_TYPES);
        if (mode < MODE_AVATARS)
        {
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR, LLPipeline::RENDER_TYPE_CONTROL_AV,
                                          LLPipeline::RENDER_TYPE_PARTICLES, LLPipeline::END_RENDER_TYPES);
        }
        if (mode < MODE_OBJECTS)
        {
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_VOLUME, LLPipeline::END_RENDER_TYPES);
        }
        if (mode < MODE_PLANTS)
        {
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_TREE, LLPipeline::RENDER_TYPE_GRASS,
                                          LLPipeline::RENDER_TYPE_PASS_GRASS, LLPipeline::END_RENDER_TYPES);
        }
    }

    // From the eye mirrored in the water, the world the mode asks for, into the pack.
    void drawMirror(U32 mode, F32 water_height, bool shadows, LLPipeline::RenderTargetPack& pack)
    {
        State& s = state();
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLViewerCamera saved_camera = *camera;
        const glm::mat4 saved_projection = get_current_projection();
        const glm::mat4 saved_modelview = get_current_modelview();
        const S32 saved_occlusion = LLPipeline::sUseOcclusion;
        const bool saved_hud = LLPipeline::sShowHUDAttachments;
        const bool saved_ui = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        LLPipeline::RenderTargetPack* const main_rt = gPipeline.mRT;
        blankUnits();   // the main view's last binds of the pack

        // The same field of view and aspect; the mirrored axes, with left made
        // again from them, so the camera stays right-handed (the picture is
        // not mirrored: the water shader projects positions, it does not flip).
        const LLVector3 eye = saved_camera.getOrigin();
        camera->setOrigin(LLVector3(eye.mV[VX], eye.mV[VY], 2.f * water_height - eye.mV[VZ]));
        camera->lookDir(mirroredInWater(saved_camera.getAtAxis()), mirroredInWater(saved_camera.getUpAxis()));
        camera->setFar(farClip());

        LLPipeline::sUseOcclusion = 0;  // the occlusion data is the main view's
        LLPipeline::sShowHUDAttachments = false;
        if (saved_ui)
        {
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        gPipeline.mRT = &pack;
        gPipeline.pushRenderTypeMask();
        maskForMode(mode);

        s.drawing = true;
        {
            // A probe face's rules (no LOD changes, no SSR, the main view's own
            // passes left out), drawn as a radiance pass (not the probes'
            // irradiance one: the sky's blues, the clouds and the ambient kept).
            plus::avatarrefl::CubeSnapshotScope cube_snapshot;
            LLSceneRecords::ViewScope scene_view(LLRHI::SCENE_VIEW_WATER);  // research/209: the retained scene's water mirror view
            LLPipeline::sProbeFaceRender = true;    // and its batch cull (LLPipeline::postSort): the batches outside the view are not drawn
            drawFace(*camera, pack, shadows);
            LLPipeline::sProbeFaceRender = false;
        }
        s.drawing = false;

        gPipeline.popRenderTypeMask();
        gPipeline.resetDrawOrders();
        gPipeline.mRT = main_rt;
        if (saved_ui && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        {
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        LLPipeline::sShowHUDAttachments = saved_hud;
        LLPipeline::sUseOcclusion = saved_occlusion;

        *camera = saved_camera;
        set_current_modelview(saved_modelview);
        set_current_projection(saved_projection);
        gViewerWindow->setup3DViewport();

        s.mode = mode;
        s.water_height = water_height;
        s.drawn_origin = eye;
        s.drawn_at = saved_camera.getAtAxis();
        s.frames_since_draw = 0;
    }

    // ------------------------------------------------------------------

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusWaterMirror*). The
        // photo tools list PlusWaterMirror (app_settings/plus/phototools/catalog.xml).
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_MODE).get())
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                          { onModeChanged((U32)llmax(0, value.asInteger())); });
        }
    }

    plus::FeatureRegistration sRegistration({ "watermirror", &init, {} });
}

namespace plus::avatarrefl::watermirror
{
    void addShaderDefines(std::map<std::string, std::string>& attribs)
    {
        const bool define = wantsShaderDefine(llmin(gSavedSettings.getU32(SETTING_MODE), (U32)MODE_AVATARS));
        // research/250: a background reload's plan leaves the programs drawn with as they are.
        if (!plus::shaderreload::planning())
        {
            State& s = state();
            s.shader_define = define;
            s.valid = false;
        }
        if (define)
        {
            attribs["PLUS_WATER_MIRROR"] = "1";
        }
    }

    void render()
    {
        State& s = state();
        const U32 mode = modeSetting();
        if (mode == MODE_OFF || !s.shader_define)
        {
            if (targets().width)
            {
                releaseTargets();
            }
            s.valid = false;
            s.fade = 0.f;
            return;
        }
        // A snapshot keeps the last frame's picture and camera: its view is
        // the same camera's, so the water still projects into it.
        if (gSnapshot)
        {
            return;
        }
        F32 water_height = 0.f;
        if (!canDraw() || !waterSeen() || !waterUnderEye(water_height))
        {
            s.valid = false;
            s.fade = 0.f;
            // Away from the water for a while (a skybox, inland): the targets go too.
            if (targets().width && ++s.idle_frames > IDLE_RELEASE_FRAMES)
            {
                releaseTargets();
            }
            return;
        }
        s.idle_frames = 0;

        static LLCachedControl<bool> shadows_setting(gSavedSettings, SETTING_SHADOWS, false);
        const bool shadows = shadows_setting;
        U32 width = 0;
        U32 height = 0;
        targetSize(width, height);
        // A picture made again while one was showing (a new scale, the shadows
        // turned on or off, a resize) is drawn in this same frame below: the
        // mirror keeps its weight rather than fading in again from nothing.
        const bool was_live = s.valid && s.fade > 0.f;
        if (!sizeTargets(width, height, shadows))
        {
            s.valid = false;
            s.fade = 0.f;
            return;
        }

        if (s.valid)
        {
            s.fade = llmin(1.f, s.fade + LLFrameTimer::getFrameDeltaTimeF32() / FADE_SECONDS);
        }
        else if (!was_live)
        {
            s.fade = 0.f;
        }
        if (!due(mode, water_height))
        {
            return;     // the last picture, read with the camera it was drawn with
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("water mirror");
        LLRHI::GPUScope gpu_scope("wmirror");
        drawMirror(mode, water_height, shadows, targets());
        s.valid = true;
    }

    void bind(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_map("plusWaterMirrorMap");
        static LLStaticHashedString s_depth("plusWaterMirrorDepth");
        static LLStaticHashedString s_matrix("plusWaterMirrorMat");
        static LLStaticHashedString s_plane("plusWaterMirrorPlane");
        static LLStaticHashedString s_params("plusWaterMirrorParams");
        static LLStaticHashedString s_filter("plusWaterMirrorFilter");
        static LLCachedControl<U32> aa_setting(gSavedSettings, SETTING_AA, AA_NONE);

        State& s = state();
        if (!s.shader_define || shader.getUniformLocation(s_map) < 0)
        {
            return;     // a program without the mirror
        }

        const bool main_view = !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT && !LLPipeline::sShadowRender &&
                               !LLPipeline::sImpostorRender && !LLPipeline::sReflectionRender && !LLPipeline::sRenderingHUDs;
        const bool water = &shader == &gWaterProgram;

        // The water pool binds its program for every frame's main view; it
        // has faces only while some of the region's water is in sight.
        if (water && main_view)
        {
            LLDrawPool* pool = gPipeline.findPool(LLDrawPool::POOL_WATER);
            if (pool && !static_cast<LLFacePool*>(pool)->mDrawFace.empty())
            {
                s.water_frame = LLFrameTimer::getFrameCount();
                s.water_frame_set = true;
            }
        }

        // The two samplers' units (MAP_UNIT, DEPTH_UNIT): a program that
        // reaches them itself goes without the mirror.
        if (shader.mActiveTextureChannels >= MAP_UNIT)
        {
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
            return;
        }
        shader.uniform1i(s_map, MAP_UNIT);
        shader.uniform1i(s_depth, DEPTH_UNIT);

        LLPipeline::RenderTargetPack& pack = targets();
        LLSettingsWater::ptr_t water_settings = LLEnvironment::instance().getCurrentWater();
        const bool live = water && main_view && s.valid && s.fade > 0.f && water_settings && pack.screen.isComplete() &&
                          pack.deferredScreen.isComplete();
        if (!live)
        {
            // Off for this program; the samplers still need something valid
            // (never the pack itself, which this may be drawing into).
            blankUnits();
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
            return;
        }

        gGL.getTexUnit(MAP_UNIT)->bind(&pack.screen);
        gGL.getTexUnit(MAP_UNIT)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(DEPTH_UNIT)->bind(&pack.deferredScreen, true);
        gGL.getTexUnit(DEPTH_UNIT)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->activate();

        // From the program's eye space (the main view's) to the mirrored camera's clip space.
        const glm::mat4 view = get_current_modelview();
        const glm::mat4 eye_to_mirror = s.view_proj * glm::inverse(view);
        shader.uniformMatrix4fv(s_matrix, 1, GL_FALSE, glm::value_ptr(eye_to_mirror));

        // The water in eye space: up * p - h = 0 with p = R^-1 (q - t).
        const glm::vec3 up_eye = glm::vec3(view * glm::vec4(0.f, 0.f, 1.f, 0.f));
        const glm::vec4 plane_eye(up_eye, -s.water_height - glm::dot(up_eye, glm::vec3(view[3])));
        shader.uniform4fv(s_plane, 1, glm::value_ptr(plane_eye));

        static LLCachedControl<F32> waves_setting(gSavedSettings, SETTING_WAVES, 1.f);
        const F32 waves_value = waves_setting;   // NaN would pass llclamp() into the shader
        const F32 waves = llclamp(std::isfinite(waves_value) ? waves_value : 1.f, 0.f, MAX_WAVES) * WAVE_GAIN * llmax(water_settings->getScaleAbove(), 0.f);
        shader.uniform4f(s_params, s.fade, waves, 0.f, 0.f);

        // The read's anti-aliasing: one texel of the picture, the mode, and the
        // picture's texels per screen pixel (above 1 the supersampling's resolve;
        // both targets' own sizes, after RenderResolutionDivisor).
        const F32 map_width = (F32)llmax(pack.screen.getWidth(), 1u);
        const F32 map_height = (F32)llmax(pack.screen.getHeight(), 1u);
        const F32 texels_per_pixel = map_width / (F32)llmax(gPipeline.mMainRT.screen.getWidth(), 1u);
        shader.uniform4f(s_filter, 1.f / map_width, 1.f / map_height, (F32)llmin((U32)aa_setting, (U32)AA_SOFT),
                         texels_per_pixel);
    }

    bool coversAvatars()
    {
        return modeSetting() >= MODE_AVATARS && state().valid;
    }

    bool clipAtWater(const glm::vec4& water_plane, glm::vec4& clip_plane)
    {
        if (!state().drawing)
        {
            return false;
        }
        clip_plane = water_plane;
        clip_plane.w += CLIP_BELOW_WATER;
        return true;
    }
}
