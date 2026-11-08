/**
 * @file avatarrefl.cpp
 * @brief Avatars in reflections (research/127). Registers with the plus
 *        foundation.
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

// Both stages are off by default.
//
// Stage 0 (PlusMirrorAvatars): a mirror (the hero probe) draws avatars only
// when its maker marked it dynamic. With this on its faces always take them,
// through the official switch for dynamic probes (force_dynamic).
//
// Stage 1 (PlusAvatarReflection; Vulkan only): one plane a frame under your
// avatar, the floor it stands on (the collision plane the server sends,
// LLVOAvatar::mFootPlane, else the terrain) or the water (the region's water
// height). Before the main view, your eye is mirrored in the plane and the
// avatars are drawn from there with an ordinary right-handed camera (the
// mirrored at and up axes), through the official deferred renderer as for a
// reflection probe face (gCubeSnapshot) but into a target pack of our own, at
// a share of the screen's size. Seen from
// the mirrored eye, a pixel of the plane and its reflection lie on the same
// ray, so the main view's probe sampling (reflectionProbeF.glsl, after the
// screen space reflections) projects the pixel's position with that camera and
// takes the avatar's colour where its depth says something was drawn beyond
// the plane. Only pixels on the plane take it.
//
// The pass draws your avatar only: it is put in the cull result directly, as
// LLPipeline::generateImpostor() does (its drawable and its attachments'
// bridges, which cull their own contents), not through updateCull(), which
// would bring the world's prims too (they share RENDER_TYPE_VOLUME with
// attachments, so the render types cannot keep them apart; research/131 H1).
// The render types are the world's minus the sky, water, terrain, plants,
// particles and HUD, as for an impostor. The sun's shadows in the pass are
// culled as usual for its cameras, so the prims around you (a roof) shade the
// reflection as they shade you.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "avatarrefl.h"
#include "watermirror.h"

#include "skinsss/skinsss.h"
#include "plus/shaderreload/shaderreload.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
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
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;          // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_MIRROR_AVATARS[] = "PlusMirrorAvatars";
    constexpr char SETTING_MODE[]           = "PlusAvatarReflection";
    constexpr char SETTING_SCALE[]          = "PlusAvatarReflectionScale";
    constexpr char SETTING_FLOOR_OFFSET[]   = "PlusAvatarReflectionFloorOffset";

    // PlusAvatarReflection: which planes may be used.
    enum Mode : U32
    {
        MODE_OFF = 0,
        MODE_FLOOR = 1,
        MODE_WATER = 2,
        MODE_BOTH = 3,
    };

    enum class Kind
    {
        None,
        Floor,      // the server's collision plane under the feet, or the terrain
        Water,
    };

    // A floor steeper than this (its normal's z) is not used.
    constexpr F32 MIN_FLOOR_UP = 0.9f;
    // The floor's height against your feet (the lower ankle joint, which is
    // some 0.1 m above the sole): from this far under them to this far over.
    constexpr F32 FLOOR_BELOW_FEET = 0.5f;
    constexpr F32 FLOOR_ABOVE_FEET = 0.2f;
    // The water is used from this far under your feet (a pier over it) up to
    // your pelvis (wading).
    constexpr F32 WATER_BELOW_FEET = 3.f;
    // Your eye must be at least this far above the plane.
    constexpr F32 MIN_EYE_ABOVE_PLANE = 0.05f;

    // How far from the plane (m) a pixel still takes the reflection, and how
    // close its normal must be to the plane's (cosine; -1: any, for the waves).
    constexpr F32 FLOOR_THICKNESS = 0.1f;
    constexpr F32 WATER_THICKNESS = 0.3f;
    constexpr F32 FLOOR_NORMAL_MIN = 0.9f;
    constexpr F32 WATER_NORMAL_MIN = -1.f;

    // A new plane (a step up or down, onto the water) fades in over this long;
    // the server's plane comes a little after the step.
    constexpr F32 FADE_SECONDS = 0.25f;

    // Your avatar's box, in which the screen space reflections' hits are
    // dropped on the plane (research/210 A2): its extents (attachments
    // included) grown by this much (m), and by how far it moved in a frame
    // (the rays hit the last frame's picture), at most MAX_SELF_MOTION.
    constexpr F32 SELF_BOX_MARGIN = 0.1f;
    constexpr F32 MAX_SELF_MOTION = 1.f;
    constexpr F32 MIN_SELF_HALF = 0.05f;

    // The plane counts as the same while it moves less than this (m).
    constexpr F32 SAME_PLANE_DISTANCE = 0.05f;

    // The mirrored camera's far clip: past your avatar's furthest point by this
    // much (m), at least MIN_FAR_CLIP, at most the probes' draw distance. Your
    // avatar is put in the pass whatever the far clip; it sets how far the
    // pass's sun shadow cascades reach (and so their texel size).
    constexpr F32 FAR_CLIP_MARGIN = 1.f;
    constexpr F32 MIN_FAR_CLIP = 2.f;

    // The texture units of the two samplers, which are not among the official
    // reserved uniforms: the last two (research/131 L5). Other plus samplers
    // outside the reserved list take units from mActiveTextureChannels up, so
    // the two never meet them while a program leaves these two free.
    constexpr S32 MAP_UNIT = (S32)LL_NUM_TEXTURE_LAYERS - 2;
    constexpr S32 DEPTH_UNIT = (S32)LL_NUM_TEXTURE_LAYERS - 1;

    constexpr F32 MIN_SCALE = 0.25f;
    constexpr F32 MAX_SCALE = 1.f;
    constexpr U32 MIN_TARGET_SIZE = 64;

    struct Plane
    {
        Kind kind = Kind::None;
        LLVector3 normal;   // unit, pointing up out of the plane
        F32 d = 0.f;        // normal * p + d = 0 (agent space)

        F32 distance(const LLVector3& p) const { return normal * p + d; }

        // The plane's height straight under (or over) p.
        F32 heightAt(const LLVector3& p) const
        {
            return (-d - normal.mV[VX] * p.mV[VX] - normal.mV[VY] * p.mV[VY]) / normal.mV[VZ];
        }

        bool sameAs(const Plane& other) const
        {
            return kind == other.kind && normal * other.normal > 0.999f && fabsf(d - other.d) < SAME_PLANE_DISTANCE;
        }
    };

    struct State
    {
        bool shader_define = false;     // the programs were built with PLUS_AVATAR_REFL
        bool valid = false;             // the target holds the avatars mirrored in plane, drawn with view_proj
        Plane plane;
        bool on_water = false;          // the water's pixels may take it (see waterTakesPlane())
        glm::mat4 view_proj{ 1.f };     // the mirrored camera's projection * view
        F32 fade = 0.f;                 // 0..1, the reflection's weight
        glm::mat4 self_box{ 1.f };      // agent space -> your avatar's box (inside: -1..1 on each axis), drawn with view_proj
        U32 screen_generation = 0;      // plus::skinsss::screenBuffersGeneration() the target was made at
    };

    State& state()
    {
        static State s;
        return s;
    }

    // The target pack, made on first use and kept while the reflection is on;
    // never destroyed by a static destructor, which would run without a GL
    // context.
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

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
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
        state().valid = false;
    }

    // The pack at width x height, made as the official viewer makes a probe's
    // or a mirror's (allocateScreenBufferInternal with gCubeSnapshot set: the
    // G-buffer, the lit screen and the sun's maps, nothing of the main view's).
    // Made again when the main view's are (a resize, the shadow or HDR settings).
    bool sizeTargets(U32 width, U32 height)
    {
        State& s = state();
        LLPipeline::RenderTargetPack& pack = targets();
        const U32 generation = plus::skinsss::screenBuffersGeneration();
        if (pack.screen.isComplete() && pack.width == width && pack.height == height && s.screen_generation == generation)
        {
            return true;
        }
        releaseTargets();

        LLPipeline::RenderTargetPack* const main_rt = gPipeline.mRT;
        gPipeline.mRT = &pack;
        gCubeSnapshot = true;
        const bool made = gPipeline.allocateScreenBufferInternal(width, height);
        gCubeSnapshot = false;
        gPipeline.mRT = main_rt;

        if (!made || !pack.screen.isComplete() || !pack.deferredScreen.isComplete())
        {
            LL_WARNS_ONCE("Plus") << "Avatar reflection: no targets of " << width << "x" << height << LL_ENDL;
            releaseTargets();
            return false;
        }
        s.screen_generation = generation;
        static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
        const LLError::ELevel log_level = resize_log.level(pack.deferredScreen.getWidth(), pack.deferredScreen.getHeight());    // once: LL_VLOGS reads its level twice
        LL_VLOGS(log_level, "Plus")
            << "Avatar reflection targets " << pack.deferredScreen.getWidth() << "x" << pack.deferredScreen.getHeight()
                         << LL_ENDL;
        return true;
    }

    U32 modeSetting()
    {
        static LLCachedControl<U32> mode(gSavedSettings, SETTING_MODE, MODE_OFF);
        return llmin((U32)mode, (U32)MODE_BOTH);
    }

    // Whether the programs should have PLUS_AVATAR_REFL for this mode.
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
    // in the background or on several threads). While a reload is on its way
    // the programs drawn with are not those it makes: asked for again (merged).
    void onModeChanged(U32 mode)
    {
        if (wantsShaderDefine(mode) == state().shader_define && !plus::shaderreload::pending())
        {
            return;
        }
        setShaders();
    }

    LLVector3 mirrored(const LLVector3& v, const LLVector3& normal)
    {
        return v - normal * (2.f * (v * normal));
    }

    // ------------------------------------------------------------------
    // The plane

    F32 lowerFootHeight(const LLVOAvatarSelf& avatar)
    {
        if (avatar.mFootLeftp && avatar.mFootRightp)
        {
            return llmin(avatar.mFootLeftp->getWorldPosition().mV[VZ], avatar.mFootRightp->getWorldPosition().mV[VZ]);
        }
        return avatar.getPositionAgent().mV[VZ] - avatar.getPelvisToFoot();
    }

    bool floorNear(const Plane& plane, const LLVector3& pelvis, F32 feet_z)
    {
        const F32 height = plane.heightAt(pelvis);
        return height >= feet_z - FLOOR_BELOW_FEET && height <= feet_z + FLOOR_ABOVE_FEET && height < pelvis.mV[VZ];
    }

    // The floor your avatar stands on: the server's collision plane (region
    // space, normal * p = w; llworld.cpp puts the ground 0.05 m under it, the
    // same as PlusAvatarReflectionFloorOffset's default), else the terrain.
    bool floorPlane(const LLVOAvatarSelf& avatar, const LLVector3& pelvis, F32 feet_z, Plane& out)
    {
        static LLCachedControl<F32> floor_offset(gSavedSettings, SETTING_FLOOR_OFFSET, -0.05f);
        LLViewerRegion* region = avatar.getRegion();
        if (!region)
        {
            return false;
        }

        if (!avatar.mFootPlane.isExactlyClear())
        {
            LLVector3 normal(avatar.mFootPlane.mV);
            const F32 length = normal.normalize();
            if (length > 0.5f && normal.mV[VZ] >= MIN_FLOOR_UP)
            {
                Plane plane;
                plane.kind = Kind::Floor;
                plane.normal = normal;
                plane.d = -(avatar.mFootPlane.mV[VW] / length + normal * region->getOriginAgent()) - floor_offset;
                if (floorNear(plane, pelvis, feet_z))
                {
                    out = plane;
                    return true;
                }
            }
        }

        LLWorld* world = LLWorld::getInstance();
        LLVector3 normal = world->resolveLandNormalGlobal(gAgent.getPosGlobalFromAgent(pelvis));
        if (normal.normalize() < 0.5f || normal.mV[VZ] < MIN_FLOOR_UP)
        {
            return false;
        }
        LLVector3 ground = pelvis;
        ground.mV[VZ] = world->resolveLandHeightAgent(pelvis);
        Plane plane;
        plane.kind = Kind::Floor;
        plane.normal = normal;
        plane.d = -(normal * ground);
        if (!floorNear(plane, pelvis, feet_z))
        {
            return false;
        }
        out = plane;
        return true;
    }

    // The region's water, from your pelvis (wading) down to a few metres under
    // your feet (a pier).
    bool waterPlane(const LLVOAvatarSelf& avatar, const LLVector3& pelvis, F32 feet_z, Plane& out)
    {
        LLViewerRegion* region = avatar.getRegion();
        if (!region)
        {
            return false;
        }
        const F32 water = region->getWaterHeight();
        if (water >= pelvis.mV[VZ] || water < feet_z - WATER_BELOW_FEET)
        {
            return false;
        }
        out.kind = Kind::Water;
        out.normal.setVec(0.f, 0.f, 1.f);
        out.d = -water;
        return true;
    }

    // Of the planes the mode allows, the highest under your pelvis.
    bool choosePlane(U32 mode, Plane& out)
    {
        const LLVOAvatarSelf& avatar = *gAgentAvatarp;
        const LLVector3 pelvis = avatar.mPelvisp ? avatar.mPelvisp->getWorldPosition() : avatar.getPositionAgent();
        const F32 feet_z = lowerFootHeight(avatar);
        // Under water to the waist or deeper: the pass clips at the water
        // (as a probe face does), so there is nothing to mirror.
        if (avatar.getRegion() && avatar.getRegion()->getWaterHeight() >= pelvis.mV[VZ])
        {
            return false;
        }

        Plane floor;
        Plane water;
        const bool has_floor = (mode & MODE_FLOOR) && floorPlane(avatar, pelvis, feet_z, floor);
        // The water mirror with the avatars in it (watermirror.cpp) already shows yours on the water.
        const bool has_water = (mode & MODE_WATER) && !plus::avatarrefl::watermirror::coversAvatars() &&
                               waterPlane(avatar, pelvis, feet_z, water);
        if (has_floor && (!has_water || floor.heightAt(pelvis) >= water.heightAt(pelvis)))
        {
            out = floor;
        }
        else if (has_water)
        {
            out = water;
        }
        else
        {
            return false;
        }
        // Seen from under the plane there is nothing to see in it.
        return out.distance(LLViewerCamera::getInstance()->getOrigin()) >= MIN_EYE_ABOVE_PLANE;
    }

    // Whether the water's pixels may take the reflection in plane. The water's
    // own plane: yes. A floor's: only where the mode also asks for the water
    // and the water mirror does not draw your avatar there itself. Otherwise a
    // floor at the water's height (within FLOOR_THICKNESS) put a still copy on
    // the sea: "floor only" must leave the water alone, and with the water
    // mirror's avatars (PlusWaterMirror 4) it was a second, unrippled one.
    bool waterTakesPlane(U32 mode, const Plane& plane)
    {
        return plane.kind == Kind::Water || ((mode & MODE_WATER) && !plus::avatarrefl::watermirror::coversAvatars());
    }

    // ------------------------------------------------------------------
    // The pass

    // Whether the pass may run in this frame at all.
    bool canDraw()
    {
        return LLRHI::sVulkanGL && state().shader_define && LLPipeline::sReflectionProbesEnabled && !gCubeSnapshot &&
               !gTeleportDisplay && !gDisconnected && LLStartUp::getStartupState() >= STATE_STARTED &&
               !LLAppViewer::instance()->logoutRequestSent() && isAgentAvatarValid() && gAgentAvatarp->mDrawable.notNull() &&
               // First person: your body is not drawn in the main view either.
               !gAgentCamera.cameraMouselook() &&
               // Under water the planes are out of sight (and the probe pass clips at the water).
               !LLViewerCamera::getInstance()->cameraUnderWater();
    }

    // Past your avatar's furthest point along the mirrored camera's view.
    F32 farClip(const LLCamera& camera)
    {
        static LLCachedControl<F32> probe_draw_distance(gSavedSettings, "RenderReflectionProbeDrawDistance", 64.f);
        const LLVector4a* extents = gAgentAvatarp->mDrawable->getSpatialExtents();
        const LLVector3 origin = camera.getOrigin();
        const LLVector3 at = camera.getAtAxis();
        F32 furthest = 0.f;
        for (U32 corner = 0; corner < 8; ++corner)
        {
            const LLVector3 p(extents[(corner & 1) ? 1 : 0].getF32ptr()[VX],
                              extents[(corner & 2) ? 1 : 0].getF32ptr()[VY],
                              extents[(corner & 4) ? 1 : 0].getF32ptr()[VZ]);
            furthest = llmax(furthest, (p - origin) * at);
        }
        return llclamp(furthest + FAR_CLIP_MARGIN, MIN_FAR_CLIP, llmax((F32)probe_draw_distance, MIN_FAR_CLIP));
    }

    // Your avatar into the cull result (sCull), as generateImpostor() puts an
    // avatar: its drawable first (an attachment's bridge is left out while its
    // avatar was not seen in the last frame), then each attachment's bridge,
    // which marks its own groups not culled for this camera. HUD attachments
    // are not drawn.
    void markYourAvatarVisible(LLCamera& camera)
    {
        LLVOAvatarSelf* avatar = gAgentAvatarp;
        gPipeline.markVisible(avatar->mDrawable, camera);
        for (const auto& [id, attachment] : avatar->mAttachmentPoints)
        {
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (LLPointer<LLViewerObject>& attached : attachment->mAttachedObjects)
            {
                if (attached.notNull() && attached->mDrawable.notNull())
                {
                    gPipeline.markVisible(attached->mDrawable->getSpatialBridge(), camera);
                }
            }
        }
    }

    // display_cube_face() (llviewerdisplay.cpp) for a target of our own size:
    // the camera is set up here rather than by display_update_camera(), which
    // takes the main view's rectangle. The mirrored eye is often under the
    // water: the pass is drawn as from above it, so the avatars over the water
    // are not clipped away.
    void drawFace(LLViewerCamera& camera, LLPipeline::RenderTargetPack& pack)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("avatar reflection face");

        LLGLSDefault gls_default;
        LLGLDepthTest gls_depth(GL_TRUE, GL_TRUE, GL_LEQUAL);

        LLVertexBuffer::unbind();
        gPipeline.disableLights();
        gPipeline.mBackfaceCull = true;

        const S32 width = pack.deferredScreen.getWidth();
        const S32 height = pack.deferredScreen.getHeight();
        camera.setPerspective(NOT_FOR_SELECTION, 0, 0, width, height, false, camera.getNear(), MAX_FAR_CLIP * 2.f);
        state().view_proj = get_current_projection() * get_current_modelview();

        LLEnvironment::instance().update(&camera);

        LLSpatialGroup::sNoDelete = true;

        static LLCullResult result;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        LLPipeline::sUnderWaterRender = false;
        result.clear();
        gPipeline.grabReferences(result);
        markYourAvatarVisible(camera);

        gGL.setColorMask(true, true);
        LLRHI::clearColor(0.f, 0.f, 0.f, 0.f);
        gPipeline.generateSunShadow(camera);
        // generateSunShadow leaves the last shadow camera current; sort for the world camera
        // (as display() does), so setVisible marks this view's slot
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

    // From your eye mirrored in the plane, the avatars only, into the pack.
    void drawReflection(const Plane& plane, LLPipeline::RenderTargetPack& pack)
    {
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
        // not mirrored: the composite projects positions, it does not flip).
        const LLVector3 eye = saved_camera.getOrigin();
        camera->setOrigin(eye - plane.normal * (2.f * plane.distance(eye)));
        camera->lookDir(mirrored(saved_camera.getAtAxis(), plane.normal), mirrored(saved_camera.getUpAxis(), plane.normal));
        camera->setFar(farClip(*camera));

        LLPipeline::sUseOcclusion = 0;  // the occlusion data is the main view's
        LLPipeline::sShowHUDAttachments = false;
        if (saved_ui)
        {
            LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        gPipeline.mRT = &pack;
        // What generateImpostor() hides of the world for an avatar (animesh
        // kept), and the water exclusion. The attachments' own types (VOLUME,
        // the pools and the rigged passes) stay: with only AVATAR and
        // CONTROL_AV, the jelly doll mask, just the system body was drawn
        // (research/131 H1).
        gPipeline.pushRenderTypeMask();
        gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY, LLPipeline::RENDER_TYPE_TERRAIN,
                                      LLPipeline::RENDER_TYPE_GRASS, LLPipeline::RENDER_TYPE_TREE, LLPipeline::RENDER_TYPE_VOIDWATER,
                                      LLPipeline::RENDER_TYPE_WATER, LLPipeline::RENDER_TYPE_WATEREXCLUSION,
                                      LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER, LLPipeline::RENDER_TYPE_PASS_GRASS,
                                      LLPipeline::RENDER_TYPE_HUD, LLPipeline::RENDER_TYPE_PARTICLES, LLPipeline::RENDER_TYPE_CLOUDS,
                                      LLPipeline::RENDER_TYPE_HUD_PARTICLES, LLPipeline::END_RENDER_TYPES);

        {
            // A probe face's rules (far clip, no LOD changes, no SSR, the main
            // view's own passes left out), drawn as a radiance pass.
            plus::avatarrefl::CubeSnapshotScope cube_snapshot;
            LLSceneRecords::ViewScope scene_view(LLRHI::SCENE_VIEW_COUNT);  // research/209: none of the retained scene's views
            drawFace(*camera, pack);
        }

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
    }

    // Agent space -> your avatar's box grown by the margin and its motion in a
    // frame (inside: -1..1 on each axis).
    glm::mat4 selfBox()
    {
        const LLVector4a* extents = gAgentAvatarp->mDrawable->getSpatialExtents();
        const F32 speed = gAgentAvatarp->getVelocity().length();
        const F32 motion = speed * LLFrameTimer::getFrameDeltaTimeF32();
        const F32 grow = SELF_BOX_MARGIN + (std::isfinite(motion) ? llclamp(motion, 0.f, MAX_SELF_MOTION) : MAX_SELF_MOTION);
        glm::vec3 center;
        glm::vec3 half;
        for (U32 axis = 0; axis < 3; ++axis)
        {
            const F32 lo = extents[0].getF32ptr()[axis];
            const F32 hi = extents[1].getF32ptr()[axis];
            center[axis] = 0.5f * (lo + hi);
            half[axis] = llmax(0.5f * (hi - lo), MIN_SELF_HALF) + grow;
        }
        return glm::scale(glm::mat4(1.f), 1.f / half) * glm::translate(glm::mat4(1.f), -center);
    }

    // Whether the reflection is drawn into the main view's programs now.
    bool live(const State& s)
    {
        LLPipeline::RenderTargetPack& pack = targets();
        return s.valid && s.fade > 0.f && !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT && !LLPipeline::sShadowRender &&
               !LLPipeline::sImpostorRender && !LLPipeline::sReflectionRender && !LLPipeline::sRenderingHUDs &&
               pack.screen.isComplete() && pack.deferredScreen.isComplete();
    }

    // The plane in the main view's eye space (view: its modelview): n * p + d = 0 with p = R^-1 (q - t).
    glm::vec4 planeEye(const State& s, const glm::mat4& view)
    {
        const LLVector3& n = s.plane.normal;
        const glm::vec3 normal_eye = glm::vec3(view * glm::vec4(n.mV[VX], n.mV[VY], n.mV[VZ], 0.f));
        return glm::vec4(normal_eye, s.plane.d - glm::dot(normal_eye, glm::vec3(view[3])));
    }

    // The last frame's eye space (what the screen space reflections' rays
    // hit) -> your avatar's box: to this frame's eye space, then to agent space.
    glm::mat4 selfBoxFromLastEye(const State& s, const glm::mat4& inverse_view)
    {
        return s.self_box * inverse_view * gGLDeltaModelView;
    }

    // ------------------------------------------------------------------

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusMirrorAvatars, PlusAvatarReflection*).
        // The photo tools list PlusAvatarReflection (app_settings/plus/phototools/catalog.xml).
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_MODE).get())
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                          { onModeChanged((U32)llmax(0, value.asInteger())); });
        }
    }

    plus::FeatureRegistration sRegistration({ "avatarrefl", &init, {} });
}

namespace plus::avatarrefl
{
    CubeSnapshotScope::CubeSnapshotScope()
        : mSavedCubeSnapshot(gCubeSnapshot), mSavedRadiancePass(gPipeline.mReflectionMapManager.isRadiancePass())
    {
        gCubeSnapshot = true;
        gPipeline.mReflectionMapManager.plusSetRadiancePass(true);
    }

    CubeSnapshotScope::~CubeSnapshotScope()
    {
        gPipeline.mReflectionMapManager.plusSetRadiancePass(mSavedRadiancePass);
        gCubeSnapshot = mSavedCubeSnapshot;
    }

    bool mirrorAvatars()
    {
        static LLCachedControl<bool> mirror_avatars(gSavedSettings, SETTING_MIRROR_AVATARS, false);
        return mirror_avatars;
    }

    void addShaderDefines(std::map<std::string, std::string>& attribs)
    {
        watermirror::addShaderDefines(attribs);

        const bool define = wantsShaderDefine(llmin(gSavedSettings.getU32(SETTING_MODE), (U32)MODE_BOTH));
        // research/250: a background reload's plan leaves the programs drawn with as they are.
        if (!plus::shaderreload::planning())
        {
            State& s = state();
            s.shader_define = define;
            s.valid = false;
        }
        if (define)
        {
            attribs["PLUS_AVATAR_REFL"] = "1";
        }
    }

    void renderPlanar()
    {
        // The water mirror first: with the avatars in it, the water is left to it below.
        watermirror::render();

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
        // the same camera's, so the composite still projects into it.
        if (gSnapshot)
        {
            return;
        }
        Plane plane;
        if (!canDraw() || !choosePlane(mode, plane))
        {
            s.valid = false;
            s.fade = 0.f;
            return;
        }

        static LLCachedControl<F32> scale_setting(gSavedSettings, SETTING_SCALE, 0.5f);
        const F32 scale_value = scale_setting;   // NaN would pass llclamp() and make (U32)NaN below
        const F32 scale = llclamp(std::isfinite(scale_value) ? scale_value : 0.5f, MIN_SCALE, MAX_SCALE);
        const U32 width = llmax((U32)(gPipeline.mMainRT.width * scale + 0.5f), MIN_TARGET_SIZE);
        const U32 height = llmax((U32)(gPipeline.mMainRT.height * scale + 0.5f), MIN_TARGET_SIZE);
        if (!sizeTargets(width, height))
        {
            s.valid = false;
            s.fade = 0.f;
            return;
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("avatar reflection");
        LLRHI::GPUScope gpu_scope("avrefl");

        s.fade = (s.valid && plane.sameAs(s.plane)) ? llmin(1.f, s.fade + LLFrameTimer::getFrameDeltaTimeF32() / FADE_SECONDS) : 0.f;
        s.plane = plane;
        s.on_water = waterTakesPlane(mode, plane);
        drawReflection(plane, targets());
        s.self_box = selfBox();
        s.valid = true;
    }

    bool selfSkip(F32 (&box)[16], F32 (&plane)[4], F32 (&params)[4])
    {
        const State& s = state();
        if (!s.shader_define || !live(s))
        {
            return false;
        }
        const glm::mat4 view = get_current_modelview();
        const glm::mat4 self_box = selfBoxFromLastEye(s, glm::inverse(view));
        const glm::vec4 plane_eye = planeEye(s, view);
        memcpy(box, glm::value_ptr(self_box), sizeof(box));
        memcpy(plane, glm::value_ptr(plane_eye), sizeof(plane));
        // As bind() gives the opaque lighting (the pixels the new rays are traced from).
        const bool water = s.plane.kind == Kind::Water;
        params[0] = s.fade;
        params[1] = water ? WATER_THICKNESS : FLOOR_THICKNESS;
        params[2] = water ? WATER_NORMAL_MIN : FLOOR_NORMAL_MIN;
        params[3] = 0.f;
        return true;
    }

    void bind(LLGLSLShader& shader)
    {
        static LLStaticHashedString s_map("plusAvatarReflMap");
        static LLStaticHashedString s_depth("plusAvatarReflDepth");
        static LLStaticHashedString s_matrix("plusAvatarReflMat");
        static LLStaticHashedString s_plane("plusAvatarReflPlane");
        static LLStaticHashedString s_params("plusAvatarReflParams");
        static LLStaticHashedString s_self("plusAvatarReflSelf");
        static LLStaticHashedString s_self_box("plusAvatarReflSelfBox");

        watermirror::bind(shader);

        const State& s = state();
        if (!s.shader_define || shader.getUniformLocation(s_map) < 0)
        {
            return;     // a program without the reflection
        }

        // The two samplers' units (MAP_UNIT, DEPTH_UNIT): free in nearly every
        // program; one that reaches them itself goes without the reflection
        // (as plus/avatarrefl watermirror.cpp: channels 0 .. count - 1 are the
        // program's, and other plus samplers take the count's unit up).
        if (shader.mActiveTextureChannels >= MAP_UNIT)
        {
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
            return;
        }
        shader.uniform1i(s_map, MAP_UNIT);
        shader.uniform1i(s_depth, DEPTH_UNIT);

        LLPipeline::RenderTargetPack& pack = targets();
        if (!live(s))
        {
            // Off for this program; the samplers still need something valid
            // (never the pack itself, which this may be drawing into).
            blankUnits();
            shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
            return;
        }

        gGL.getTexUnit(MAP_UNIT)->bind(&pack.screen);
        gGL.getTexUnit(MAP_UNIT)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(DEPTH_UNIT)->bind(&pack.deferredScreen, true);
        gGL.getTexUnit(DEPTH_UNIT)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

        // From the program's eye space (the main view's) to the mirrored camera's clip space.
        const glm::mat4 view = get_current_modelview();
        const glm::mat4 inverse_view = glm::inverse(view);
        const glm::mat4 eye_to_mirror = s.view_proj * inverse_view;
        shader.uniformMatrix4fv(s_matrix, 1, GL_FALSE, glm::value_ptr(eye_to_mirror));

        const glm::vec4 plane_eye = planeEye(s, view);
        shader.uniform4fv(s_plane, 1, glm::value_ptr(plane_eye));

        const bool water = s.plane.kind == Kind::Water;
        shader.uniform4f(s_params, s.fade, water ? WATER_THICKNESS : FLOOR_THICKNESS, water ? WATER_NORMAL_MIN : FLOOR_NORMAL_MIN,
                         s.on_water ? 1.f : 0.f);

        // The official screen space reflections (screenSpaceReflUtil.glsl)
        // drop their hits on your avatar where this program's pixels take
        // the reflection: not the water's under a floor's plane (as
        // reflectionProbeF.glsl's plus_avatar_refl_water).
        if (shader.getUniformLocation(s_self) >= 0)
        {
            const bool takes = &shader != &gWaterProgram || s.on_water;
            shader.uniform4f(s_self, takes ? 1.f : 0.f, 0.f, 0.f, 0.f);
            const glm::mat4 self_box = selfBoxFromLastEye(s, inverse_view);
            shader.uniformMatrix4fv(s_self_box, 1, GL_FALSE, glm::value_ptr(self_box));
        }
    }
}
