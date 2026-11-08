/**
 * @file overhead.cpp
 * @brief Rain: the overhead depth map around the camera, drawn through the
 *        official shadow caster path (research/120 section 8).
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

// The world around the camera seen from straight above, orthographic, as a
// depth map: where it holds a surface well above a point, that point is
// under a roof. The rain pass reads it for each drop and splash, the wet
// look for each pixel (llrhirain.h).
//
// What is in it: the static world the sun's shadows take (prims, meshes,
// terrain, Linden trees, alpha-masked and blended prims as their shadows
// are), drawn by LLPipeline::renderShadow() as a fifth shadow camera. Not
// avatars, animesh nor attachments: they move, and an avatar would keep the
// ground under it dry (the attachments are hidden for the pass the way
// mouselook hides them). Not grass and not water (water stops no rain;
// splashes find the water's surface in the scene's depth). Moving prims are
// in it as they stood when it was drawn.
//
// Its box: SPAN metres square around the camera (snapped to whole texel
// steps, so a redraw does not move the soft edges), from ABOVE metres over
// the camera down DEPTH metres. A skybox far overhead is above the box and
// does not count as a roof over the ground.
//
// Drawn again when the camera has moved REDRAW_DISTANCE across or
// REDRAW_HEIGHT up or down since, or REFRESH_SECONDS have passed (objects
// that rezzed or moved). Not for snapshots (they keep the frame's map) nor
// reflection probes. The culling state is the unused water camera's
// (LLViewerCamera::CAMERA_WATER0: the PBR pipeline draws no water
// reflection), so no other camera's is touched.

#include "llviewerprecompiledheaders.h"

#include "overhead.h"
#include "plus/plus.h"

#include "llagent.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    // The box (m) and the map's resolution: 9.4 cm texels.
    constexpr F32 SPAN = 96.f;
    constexpr U32 RESOLUTION = 1024;
    constexpr F32 ABOVE = 80.f;
    constexpr F32 DEPTH = 200.f;
    // The box's centre moves in steps of whole texels (32 of them, 3 m).
    constexpr F32 SNAP = SPAN / (F32)RESOLUTION * 32.f;
    // When it is drawn again (m, s).
    constexpr F32 REDRAW_DISTANCE = 12.f;
    constexpr F32 REDRAW_HEIGHT = 10.f;
    constexpr F64 REFRESH_SECONDS = 4.0;
    // A surface covers a point only when this much above it (m): a texel's
    // slope on a roof, the roof itself under its own rain. The map fades to
    // open sky over EDGE_FADE before its edge. Points of a surface are looked
    // up NORMAL_OFFSET out along their normal (a wall beside a roof's edge
    // takes the rain, a wall under the eaves does not).
    constexpr F32 COVER_BIAS = 0.3f;
    constexpr F32 EDGE_FADE = 8.f;
    constexpr F32 NORMAL_OFFSET = 0.2f;

    struct Map
    {
        LLRenderTarget target;
        bool valid = false;
        LLVector3d top_centre;      // global: the box's centre (x, y) at its top (z)
        F64 camera_z = 0.0;         // global: the camera's height when it was drawn
        F64 drawn_at = 0.0;         // s (LLFrameTimer::getElapsedSeconds)
        plus::RetryWait retry;      // after the target could not be made (VRAM full)
    };

    // Made on first use; never destroyed by a static destructor, which would
    // run without a GL context.
    Map& theMap()
    {
        static Map* map = new Map();
        return *map;
    }

    F64 snapped(F64 x)
    {
        return std::floor(x / SNAP + 0.5) * SNAP;
    }

    glm::mat4 mapProjection()
    {
        const F32 half = SPAN * 0.5f;
        return glm::ortho(-half, half, -half, half, 0.f, DEPTH);
    }

    // Agent coordinates -> the box's eye space: world axes, the eye at the
    // top of its centre looking down (-z).
    glm::mat4 mapView(const LLVector3& top_centre_agent)
    {
        return glm::translate(glm::mat4(1.f), -glm::vec3(top_centre_agent.mV[VX], top_centre_agent.mV[VY], top_centre_agent.mV[VZ]));
    }

    bool due(const Map& map, const LLVector3d& camera)
    {
        if (!map.valid || !map.target.isComplete())
        {
            return true;
        }
        const F64 dx = camera.mdV[VX] - map.top_centre.mdV[VX];
        const F64 dy = camera.mdV[VY] - map.top_centre.mdV[VY];
        return dx * dx + dy * dy > (F64)(REDRAW_DISTANCE * REDRAW_DISTANCE) ||
               std::fabs(camera.mdV[VZ] - map.camera_z) > (F64)REDRAW_HEIGHT ||
               LLFrameTimer::getElapsedSeconds() - map.drawn_at > REFRESH_SECONDS;
    }

    // Every avatar's attachments out of the cull while it lives, as mouselook
    // hides your own (LLViewerJointAttachment::setAttachmentVisibility); then
    // each gets back the type it had.
    class HiddenAttachments
    {
    public:
        HiddenAttachments()
        {
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                LLVOAvatar* avatar = (LLVOAvatar*)character;
                if (!avatar || avatar->isDead())
                {
                    continue;
                }
                for (const auto& point : avatar->mAttachmentPoints)
                {
                    LLViewerJointAttachment* attachment = point.second;
                    if (!attachment)
                    {
                        continue;
                    }
                    for (LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
                    {
                        if (object.isNull() || object->mDrawable.isNull())
                        {
                            continue;
                        }
                        LLSpatialBridge* bridge = object->mDrawable->getSpatialBridge();
                        if (bridge && bridge->mDrawableType != 0)
                        {
                            mSaved.emplace_back(bridge, bridge->mDrawableType);
                            bridge->mDrawableType = 0;
                        }
                    }
                }
            }
        }

        ~HiddenAttachments()
        {
            for (const auto& saved : mSaved)
            {
                saved.first->mDrawableType = saved.second;
            }
        }

    private:
        std::vector<std::pair<LLSpatialBridge*, U32>> mSaved;
    };

    void draw(LLCamera& camera, const LLVector3d& camera_global)
    {
        Map& map = theMap();
        if (!map.target.isComplete())
        {
            if (!map.retry.due())
            {
                return;     // it could not be made a moment ago: not again every frame
            }
            if (!map.target.allocate(RESOLUTION, RESOLUTION, 0, true))
            {
                map.target.release();
                map.retry.failed();
                LL_WARNS_ONCE("Rain") << "Rain: no overhead map at " << RESOLUTION << "x" << RESOLUTION << LL_ENDL;
                return;
            }
            map.retry.succeeded();
            LL_INFOS("Rain") << "Rain: overhead map " << RESOLUTION << "x" << RESOLUTION << ", " << SPAN << " m across" << LL_ENDL;
        }

        const LLVector3d top_centre(snapped(camera_global.mdV[VX]), snapped(camera_global.mdV[VY]), camera_global.mdV[VZ] + ABOVE);
        const LLVector3 top_agent = gAgent.getPosAgentFromGlobal(top_centre);
        const glm::mat4 view = mapView(top_agent);
        const glm::mat4 proj = mapProjection();

        // The main view's state, as generateSunShadow leaves it for what follows.
        const glm::mat4 saved_view = get_current_modelview();
        const glm::mat4 saved_proj = get_current_projection();
        const glm::mat4 saved_last_view = get_last_modelview();
        const glm::mat4 saved_last_proj = get_last_projection();
        S32 saved_viewport[4];
        memcpy(saved_viewport, gGLViewport, sizeof(saved_viewport));
        const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

        LLCamera map_camera = camera;
        map_camera.setFar(DEPTH);
        map_camera.setOriginAndLookAt(top_agent, LLVector3(0.f, 1.f, 0.f), top_agent - LLVector3(0.f, 0.f, 1.f));

        LLRHI::gpuMark("rainmap");
        set_current_modelview(view);
        set_current_projection(proj);
        map.target.bindTarget();
        map.target.getViewport(gGLViewport);
        map.target.clear();
        LLViewerCamera::updateFrustumPlanes(map_camera, false, false, true);
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WATER0;

        gPipeline.pushRenderTypeMask();
        gPipeline.andRenderTypeMask(LLPipeline::RENDER_TYPE_SIMPLE,
                                    LLPipeline::RENDER_TYPE_ALPHA,
                                    LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER,
                                    LLPipeline::RENDER_TYPE_ALPHA_POST_WATER,
                                    LLPipeline::RENDER_TYPE_GLTF_PBR,
                                    LLPipeline::RENDER_TYPE_FULLBRIGHT,
                                    LLPipeline::RENDER_TYPE_BUMP,
                                    LLPipeline::RENDER_TYPE_VOLUME,
                                    LLPipeline::RENDER_TYPE_TREE,
                                    LLPipeline::RENDER_TYPE_TERRAIN,
                                    LLPipeline::RENDER_TYPE_PASS_ALPHA,
                                    LLPipeline::RENDER_TYPE_PASS_ALPHA_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_SIMPLE,
                                    LLPipeline::RENDER_TYPE_PASS_BUMP,
                                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT,
                                    LLPipeline::RENDER_TYPE_PASS_SHINY,
                                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_SHINY,
                                    LLPipeline::RENDER_TYPE_PASS_MATERIAL,
                                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA,
                                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE,
                                    LLPipeline::RENDER_TYPE_PASS_SPECMAP,
                                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_BLEND,
                                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_EMISSIVE,
                                    LLPipeline::RENDER_TYPE_PASS_NORMMAP,
                                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_BLEND,
                                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_EMISSIVE,
                                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC,
                                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_BLEND,
                                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_MASK,
                                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_EMISSIVE,
                                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR,
                                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK,
                                    LLPipeline::END_RENDER_TYPES);
        gGL.setColorMask(false, false);
        {
            HiddenAttachments hidden;
            static LLCullResult result;
            gPipeline.renderShadow(view, proj, map_camera, result, false);
        }
        gGL.setColorMask(true, true);
        gPipeline.popRenderTypeMask();
        map.target.flush();

        LLViewerCamera::sCurCameraID = saved_camera;
        memcpy(gGLViewport, saved_viewport, sizeof(saved_viewport));
        set_current_modelview(saved_view);
        set_current_projection(saved_proj);
        set_last_modelview(saved_last_view);
        set_last_projection(saved_last_proj);
        LLRHI::gpuMark("shadow");   // the GPU ms line: back to the segment it interrupted

        map.valid = true;
        map.top_centre = top_centre;
        map.camera_z = camera_global.mdV[VZ];
        map.drawn_at = LLFrameTimer::getElapsedSeconds();
    }
}

namespace plus::rain::overhead
{
    void update(LLCamera& camera)
    {
        if (gCubeSnapshot)
        {
            return;
        }
        const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera.getOrigin());
        if (due(theMap(), camera_global))
        {
            draw(camera, camera_global);
        }
    }

    void release()
    {
        Map& map = theMap();
        map.valid = false;
        if (map.target.isComplete())
        {
            map.target.release();
            LL_INFOS("Rain") << "Rain: overhead map released" << LL_ENDL;
        }
    }

    LLRHI::RainOverhead lookup(const glm::mat4& to_agent, const LLVector2& rain_slope)
    {
        LLRHI::RainOverhead out;
        const Map& map = theMap();
        if (!map.valid || !map.target.isComplete() || !map.target.getDepth())
        {
            return out;
        }
        // Where the box is now in agent coordinates (the agent frame moves between regions).
        const glm::mat4 to_map = mapProjection() * mapView(gAgent.getPosAgentFromGlobal(map.top_centre)) * to_agent;
        memcpy(out.to_map, glm::value_ptr(to_map), sizeof(out.to_map));
        out.depth_texture = map.target.getDepth();
        out.cover_bias = COVER_BIAS * 2.f / DEPTH;
        out.edge = EDGE_FADE * 2.f / SPAN;
        out.normal_offset = NORMAL_OFFSET;
        // Slanted rain that reaches a point fell from upwind of it.
        out.rain_shift[0] = -rain_slope.mV[VX] * 2.f / SPAN;
        out.rain_shift[1] = -rain_slope.mV[VY] * 2.f / SPAN;
        return out;
    }
}
