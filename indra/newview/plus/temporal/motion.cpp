/**
 * @file motion.cpp
 * @brief The main view's motion vectors (research/373 section 2): the G-buffer's
 *        fifth attachment, the camera's motion filled in after the G-buffer,
 *        and a view of them (LLRHI_MOTION_VIEW).
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "motion.h"
#include "temporal.h"
#include "plus/shaderreload/shaderreload.h"
#include "plus/upscale/upscale.h"
#include "presets/presets.h"

#include "llcallbacklist.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhimotion.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    // The main view's G-buffer and its motion attachment (main thread).
    struct State
    {
        // The programs drawn with were made with HAS_MOTION (the last shader load that was not a plan pass).
        bool shaders = false;
        // The attachment is the main view's G-buffer's MOTION_ATTACHMENT (made at the last allocation; only while
        // shaders).
        bool made = false;
        // The programs have HAS_MOTION and the attachment could not be added at the last allocation (VRAM, a
        // G-buffer of another make): not asked for again until the screen targets are made again for another
        // reason, or the shaders are made again.
        bool failed = false;
        // The shaders were asked for again (setShaders) since motionWanted() stopped matching them: not every frame
        // until the reload is committed (addShaderDefines then), again after RetryWait's seconds when none came
        // (setShaders returns early while LLViewerShaderMgr::sSkipReload is set).
        plus::RetryWait shaders_asked;
        // The screen targets were asked for again (requestResizeScreenTexture) since the attachment went missing
        // under programs that write it: once, not every frame until the allocation comes.
        bool asked_targets = false;
        // gFrameCount of the last frame whose camera pass drew (U32_MAX: none).
        U32 drawn_frame = U32_MAX;
        // gFrameCount of the last frame whose motion attachment was cleared (U32_MAX: none): the camera's pass draws
        // only on a frame whose clear went through (otherwise the clear's magenta, w 1, may be left).
        U32 cleared_frame = U32_MAX;
        bool clear_failure_logged = false;
        // The last drawn frame's camera (the view, and the projection without the jitter).
        plus::temporal::FrameHistory history;
        // The lines of the attachment made or not made (once a second while the window frame is dragged).
        plus::ResizeLog made_log;
        plus::ResizeLog failed_log;
    };

    // Never destroyed: the attachment is the G-buffer's, released with it (LLPipeline::releaseScreenBuffers).
    State& state()
    {
        static State* state = new State();
        return *state;
    }

    bool emissiveAttachment()
    {
        static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
        return emissive;
    }

    // The main view being drawn (not a probe's face, a mirror, or a snapshot's own targets).
    bool mainView()
    {
        return !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT;
    }

    // The main view's G-buffer has the attachment as it was made (complete, with its depth); what the clear and the
    // camera's pass both draw on.
    bool hasMotionAttachment(const LLRenderTarget& gbuffer)
    {
        return gbuffer.isComplete() && gbuffer.getNumTextures() == LLRHI::MOTION_ATTACHMENT + 1 && gbuffer.getDepth();
    }

    // The main view's G-buffer with the attachment, as it is now (not bound).
    LLRenderTarget* motionTarget()
    {
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        return hasMotionAttachment(gbuffer) && !gbuffer.isBoundInStack() ? &gbuffer : nullptr;
    }

    // Each frame at idle (the cached settings motionWanted reads and a few flags; nothing else while all agree):
    // - motionWanted() is not what the programs have: the shaders made again, once (they make the screen targets
    //   again too, and addMotionAttachment follows the programs);
    // - the programs write the vectors and the G-buffer lacks the attachment (not after a failure): the screen
    //   targets asked for again, once.
    bool onIdle()
    {
        State& s = state();
        if (plus::presets::snapshotInProgress())
        {
            return false;   // repeat: nothing is made again in the middle of a snapshot (its G-buffer has no attachment)
        }
        if (plus::temporal::motionWanted() != s.shaders)
        {
            if (s.shaders_asked.due() && !plus::shaderreload::pending())
            {
                s.shaders_asked.failed();   // the wait before asking again (a commit ends it)
                LL_INFOS("Plus") << "Motion vectors: " << (s.shaders ? "no longer wanted" : "wanted") << "; the shaders are made again"
                                 << LL_ENDL;
                LLViewerShaderMgr::instance()->setShaders();
            }
            return false;   // repeat
        }
        s.shaders_asked.succeeded();
        if (!s.shaders || s.made || s.failed)
        {
            s.asked_targets = false;
            return false;   // repeat
        }
        const LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
        if (!s.asked_targets && rt == &gPipeline.mMainRT && rt->width && rt->height)
        {
            s.asked_targets = true;
            LL_INFOS("Plus") << "Motion vectors: the G-buffer has no attachment for them; the screen targets are made again" << LL_ENDL;
            gPipeline.requestResizeScreenTexture();
        }
        return false;       // repeat
    }

    void onUIReady()
    {
        doOnIdleRepeating(&onIdle);
    }

    plus::FeatureRegistration sRegistration({ "temporal-motion", nullptr, &onUIReady });
}

namespace plus::temporal
{
    bool motionWanted()
    {
        return LLRHI::sMotion && LLRHI::sMotionCamera && emissiveAttachment() &&
               (LLRHI::sMotionViewMode != LLRHI::MOTION_VIEW_OFF || plus::upscale::temporalSelected());
    }

    void addShaderDefines(std::map<std::string, std::string>& attribs)
    {
        const bool define = motionWanted();
        // research/250: a background reload's plan leaves the programs drawn with as they are.
        if (!plus::shaderreload::planning())
        {
            State& s = state();
            s.shaders = define;
            s.failed = false;       // the screen targets follow (setShaders makes them again)
            // Every G-buffer is bound with a slot for the fifth output while the programs write it.
            LLRenderTarget::sPaddedDrawBuffers = define ? LLRHI::MOTION_ATTACHMENT + 1 : 0;
        }
        if (define)
        {
            attribs["HAS_MOTION"] = "1";
        }
    }

    void addMotionAttachment(LLRenderTarget& gbuffer)
    {
        State& s = state();
        s.made = false;
        s.drawn_frame = U32_MAX;
        s.cleared_frame = U32_MAX;
        s.history.drop();
        if (!s.shaders)
        {
            s.failed = false;
            return;         // the programs do not write them: no attachment either
        }
        if (plus::presets::snapshotInProgress())
        {
            // A snapshot's G-buffer (its size, then the window's again at its end) has none: no VRAM for vectors nothing
            // reads there, nor a failure of it among the snapshot's. Its fifth output goes into the padded slot. failed is
            // left as it was: after the snapshot onIdle asks for the window's attachment again only when it had one (a
            // failure before stays one, and is not tried again for the snapshot's sake).
            return;
        }
        // The attachment's index is fixed (the shaders write location 4): the G-buffer has the four before it.
        if (gbuffer.getNumTextures() != LLRHI::MOTION_ATTACHMENT || !gbuffer.addColorAttachment(GL_RGBA16F))
        {
            s.failed = true;
            const LLError::ELevel seen = s.failed_log.level(gbuffer.getWidth(), gbuffer.getHeight(), gbuffer.getNumTextures());
            const LLError::ELevel level = seen == LLError::LEVEL_INFO ? LLError::LEVEL_WARN : seen;    // a local: LL_VLOGS reads it twice
            LL_VLOGS(level, "Plus")
                << "Motion vectors: none made for the " << gbuffer.getWidth() << "x" << gbuffer.getHeight() << " G-buffer of "
                << gbuffer.getNumTextures() << " attachments (" << LLRHI::MOTION_ATTACHMENT
                << " before it are needed, and the VRAM); off until the screen targets are made again" << LL_ENDL;
            return;
        }
        s.made = true;
        s.failed = false;
        const LLError::ELevel level = s.made_log.level(gbuffer.getWidth(), gbuffer.getHeight());
        LL_VLOGS(level, "Plus") << "Motion vectors: made at " << gbuffer.getWidth() << "x" << gbuffer.getHeight() << LL_ENDL;
    }

    void clearMotion()
    {
        State& s = state();
        if (!s.made || !mainView() || !LLRHI::sMotionClear)
        {
            return;         // off: the one test
        }
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (LLRenderTarget::getCurrentBoundTarget() == &gbuffer && hasMotionAttachment(gbuffer) &&
            LLRHI::sMotionClear(gbuffer.getWidth(), gbuffer.getHeight()))
        {
            s.cleared_frame = gFrameCount;
            return;
        }
        if (!s.clear_failure_logged)
        {
            s.clear_failure_logged = true;
            LL_DEBUGS("Plus") << "Motion vectors: the attachment was not cleared (the G-buffer not bound or not as made); "
                              << "the camera's pass waits for a frame that is" << LL_ENDL;
        }
    }

    void releaseMotion()
    {
        State& s = state();
        s.made = false;
        s.drawn_frame = U32_MAX;
        s.cleared_frame = U32_MAX;
        s.history.drop();
    }

    void renderMotion()
    {
        State& s = state();
        if (!s.made)
        {
            return;         // off: the one test
        }
        if (!mainView())
        {
            return;         // a probe's face or a mirror (no attachment there): the main view's last camera is left as it is
        }
        LLRenderTarget* gbuffer = plus::presets::snapshotInProgress() || s.cleared_frame != gFrameCount ? nullptr : motionTarget();
        if (!gbuffer)
        {
            // a snapshot is drawn once, in tiles, at another size; the attachment was not cleared this frame; or the
            // G-buffer is not as made
            s.history.drop();
            s.drawn_frame = U32_MAX;
            return;
        }
        const U32 width = gbuffer->getWidth();
        const U32 height = gbuffer->getHeight();
        // This frame's camera, loaded for the G-buffer (the projection jittered while a time-based pass draws).
        const glm::mat4 view = get_current_modelview();
        const glm::mat4 drawn = get_current_projection();
        const glm::mat4 unmoved = plus::temporal::jitteredThisFrame() ? plus::temporal::unmovedProjection() : drawn;

        LLRHI::MotionCameraPass pass;
        pass.depth_texture = gbuffer->getDepth();
        pass.width = width;
        pass.height = height;
        const glm::mat4 inverse_drawn = glm::inverse(drawn);
        memcpy(pass.inverse_projection, glm::value_ptr(inverse_drawn), sizeof(pass.inverse_projection));
        memcpy(pass.projection, glm::value_ptr(unmoved), sizeof(pass.projection));
        pass.has_history = s.history.usable(width, height, view);
        if (pass.has_history)
        {
            const glm::mat4 reproject = s.history.reprojectFrom(view);
            memcpy(pass.reproject, glm::value_ptr(reproject), sizeof(pass.reproject));
        }

        LLRHI::gpuMark("motion");
        gGL.flush();
        gbuffer->bindTarget();
        const bool recorded = LLRHI::sMotionCamera(pass);
        gbuffer->flush();
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest of the lighting goes back to its segment
        if (!recorded)
        {
            s.history.drop();
            s.drawn_frame = U32_MAX;
            return;
        }
        s.history.commit(width, height, view, unmoved);
        s.drawn_frame = gFrameCount;
    }

    void viewMotion()
    {
        if (LLRHI::sMotionViewMode == LLRHI::MOTION_VIEW_OFF)
        {
            return;         // off: the one test
        }
        const U32 motion = motionTexture();
        if (!motion || !LLRHI::sMotionViewDraw)
        {
            return;         // not made this frame (a snapshot, a probe's face, the pass failed)
        }
        const LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        LLRHI::MotionViewPass pass;
        pass.motion_texture = motion;
        pass.motion_width = gbuffer.getWidth();
        pass.motion_height = gbuffer.getHeight();
        pass.mode = LLRHI::sMotionViewMode;
        LLRHI::gpuMark("motion view");
        LLRHI::sMotionViewDraw(pass);      // on the bound target, as the present pass drew
        LLRHI::gpuMark("post");             // the GPU ms line: back to renderFinalize's segment
    }

    U32 motionTexture()
    {
        const State& s = state();
        if (!s.made || s.drawn_frame != gFrameCount || !mainView())
        {
            return 0;
        }
        const LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        return gbuffer.getNumTextures() == LLRHI::MOTION_ATTACHMENT + 1 ? gbuffer.getTexture(LLRHI::MOTION_ATTACHMENT) : 0;
    }
}
