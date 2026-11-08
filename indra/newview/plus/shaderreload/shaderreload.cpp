/**
 * @file shaderreload.cpp
 * @brief A settings change's shader reload: made in the background while the
 *        frames go on with the shaders in use (research/250), or compiled on
 *        several threads with the old buffers freed between its passes; the
 *        official mirror workaround's second reload skipped with Vulkan
 *        (fix-round107).
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

// A change in the advanced graphics settings (shadows, reflections, mirrors,
// SSAO...) reloads every shader through LLViewerShaderMgr::setShaders(),
// which links some 200 programs one after another: a combination of settings
// met for the first time compiled them all on the main thread (2026-10-24 log:
// 207 programs, 14.6 s, about 40 s with the rest). Only the presets, the
// settings import and the avatar/water reflections used the parallel reload
// (research/111 B-SC: the compiles go to worker threads in a first pass, the
// second pass links). This hook sends every reload after login through it.
// LLRHI_PARALLEL_SETSHADERS=0 keeps the one-pass reload.
//
// Each pass (and a one-pass reload) first lets the frames in flight finish,
// so the render targets the caller or the previous pass released are freed
// before the new ones are made. The frame is always open between presents and
// a release waits for it, so two or three reloads in a row (the setting's own
// release, the two passes, the mirror workaround below) held as many sets of
// render targets at once: 2.4 GB -> 6.2 GB, over the 6.6 GB budget, on the
// same day's log.
//
// gfxadv-close: closing the advanced graphics floater with its X cancels the
// preferences (official), putting back each setting changed since they
// opened. Every one of them with a reloading handler (shadows, SSAO,
// reflections, mirrors, SSR, transparent water, vintage mode...) reloaded
// all the shaders by itself: six reloads in 25 s on the 2026-10-30 log, VRAM
// 4.7 -> 8.1 GB of a 6.7 GB budget, then a long stall. A Batch around the
// cancel notes the reloads and the target remakes and makes one at its end.
//
// research/250: even in parallel, a reload stood the viewer still for 2 to 7
// seconds (`Parallel shader reload in 4.59 s ... 496 builds, 248 compiled
// (1.8 s of compiling)`): most of it the two passes of setShaders themselves
// on the main thread. After login a reload is now made in the background:
//  1. The request (a reloading setting's handler, the end of a Batch, a
//     quality level) makes the render targets for the new settings at once,
//     as the reload's own release and create would, and returns: the shaders
//     drawn with stay. A second request within SETTLE_SECONDS is the same one.
//  2. The plan: setShaders runs once with LLGLSLShader::sPlan set. Every
//     createShader hands its program's sources to the Vulkan backend's build
//     threads (LLRHI::sShaderReloadBuilder) and leaves the shader as it is;
//     PlanScope keeps and puts back what else the pass sets.
//  3. The build: low priority threads make all a link makes of each
//     program's sources, while the frames go on with the programs in use
//     (and a notice at the bottom of the world says how far it is).
//  4. The commit: at the head of the frame after the last build, a plain
//     setShaders whose links take those builds. A newer request on the way
//     drops the plan and plans again; when a program did not build, the
//     reload is made now the usual way (which may make it at a lower shader
//     level; the notice says so only if that gives up on one too).
// The settings that change which programs are made or what they write to
// (Structure) go the usual way, as does everything before login, in a
// snapshot, with the shaders gone (restoreGL) or with LLRHI_ASYNC_SHADER_RELOAD=0.

#include "llviewerprecompiledheaders.h"

#include "shaderreload.h"
#include "reloadnotice.h"

#include "llapp.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrhi.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "plus/presets/presets.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <unordered_set>

extern bool gSnapshot;
extern bool gCubeSnapshot;

namespace plus::shaderreload
{
namespace
{
    const bool sParallelSetShaders = LLRHI::envFirstChar("LLRHI_PARALLEL_SETSHADERS") != '0';

    // research/250: LLRHI_ASYNC_SHADER_RELOAD=0 keeps every reload on the main thread, as before.
    const bool sBackgroundReload = LLRHI::envFirstChar("LLRHI_ASYNC_SHADER_RELOAD") != '0';

    void setShaders()
    {
        LLViewerShaderMgr::instance()->setShaders();
    }

    // After login and outside a snapshot's frames: the main loop between
    // frames, a settings signal, a menu, an idle callback (display() itself
    // reloads only in checkSettings, before its draws). Before login the
    // first loads stay as they were.
    bool atSafePoint()
    {
        return LLRHI::sVulkanGL && !gSnapshot && !gCubeSnapshot && LLStartUp::getStartupState() >= STATE_STARTED;
    }

    // The targets go, the frames in flight free them, and the new ones are
    // made: one set of render targets at a time.
    void remakeTargets()
    {
        gPipeline.releaseGLBuffers();
        if (atSafePoint())
        {
            LLRHI::freeDeferred();
        }
        gPipeline.createGLBuffers();
    }

    // gfxadv-close: the open batches and what their settings asked for.
    U32 sBatchDepth = 0;
    U32 sReloadsAsked = 0;
    U32 sRemakesAsked = 0;

    // What the batch's settings asked for, once: the reload remakes the
    // render targets itself (setShaders releases them first and makes them
    // last). The handlers' other work (LLPipeline::refreshCachedSettings, the
    // probe managers' reset, the bump maps) ran at each setting, as before.
    void flush()
    {
        const U32 reloads = sReloadsAsked;
        const U32 remakes = sRemakesAsked;
        sReloadsAsked = 0;
        sRemakesAsked = 0;
        if (!reloads && !remakes)
        {
            return;
        }
        LL_INFOS("Plus") << "Settings changed together: " << reloads << " shader reloads and " << remakes
                         << " render target remakes asked for, " << (reloads ? "one reload" : "one remake") << " made" << LL_ENDL;
        if (!gPipeline.isInit())
        {
            return;
        }
        if (reloads)
        {
            LLPipeline::refreshCachedSettings();    // as handleSetShaderChanged, with every value back
            setShaders();
            return;
        }
        remakeTargets();
    }

    // ------------------------------------------------------------------
    // research/250: the background reload

    constexpr F32 SETTLE_SECONDS = 0.2f;    // a request waits this long for the next one before the plan
    constexpr F32 FAILURE_SECONDS = 6.f;    // the notice of a failed build stays this long

    // The settings that change which programs setShaders makes (GLTF's
    // variants, the terrain paint's programs) or what the G-buffer programs
    // write to (the emissive buffer, with HDR in the vintage mode), and the
    // OIT sources the programs keep. Drawn with programs made without them
    // for a few frames, bind(variant) would assert or the outputs would not
    // match the targets: a change of one goes the usual way.
    struct Structure
    {
        bool gltf = false;
        bool terrain_paint = false;
        bool emissive = false;
        bool hdr = false;
        bool oit_sources = false;

        bool operator==(const Structure& other) const
        {
            return gltf == other.gltf && terrain_paint == other.terrain_paint && emissive == other.emissive && hdr == other.hdr &&
                   oit_sources == other.oit_sources;
        }
        bool operator!=(const Structure& other) const { return !(*this == other); }
    };

    // Read at a reload only (by name: not each frame).
    Structure currentStructure()
    {
        Structure structure;
        structure.gltf = gSavedSettings.getBOOL("GLTFEnabled");
        structure.terrain_paint = gSavedSettings.getBOOL("LocalTerrainPaintEnabled");
        structure.emissive = gSavedSettings.getBOOL("RenderEnableEmissiveBuffer");
        structure.hdr = gSavedSettings.getBOOL("RenderHDREnabled");
        structure.oit_sources = LLRHI::sOITSources;
        return structure;
    }

    enum class Phase
    {
        Idle,
        Settling,   // asked for: the plan waits for the requests to stop
        Building,   // planned: the build threads make the programs
        Ready,      // built: the commit at the next idle (the notice says so in between)
        Failed,     // made the usual way after a failed build, and that gave up on a program too; or a build threw
                    // (not made the usual way, research/275 L062): the notice for a while
    };

    struct Background
    {
        Phase phase = Phase::Idle;
        bool ticking = false;       // tick() is in the idle callbacks
        bool committing = false;    // the commit's setShaders
        bool commit_ran = false;    // ... got past its early returns
        U32 made_now = 0;           // reloads made the usual way under way (their passes come back to beforeReload)
        bool live_known = false;
        Structure live;             // of the programs drawn with
        U32 requests = 0;           // since the plan
        U32 build_frame = 0;        // LLFrameTimer::getFrameCount() when the build started
        F64 plan_seconds = 0.0;
        LLFrameTimer since;         // since the last request (Settling), the failure (Failed)
        LLTimer whole;              // since the first request
    };
    Background sBg;

    LLRHI::ShaderReloadBuilder* builder()
    {
        return LLRHI::sShaderReloadBuilder;
    }

    // What the reload about to be made will draw with.
    void noteLive()
    {
        sBg.live = currentStructure();
        sBg.live_known = true;
    }

    bool overBudget()
    {
        U64 textures = 0, usage = 0, budget = 0;
        return LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(textures, usage, budget) && budget > 0 && usage >= budget;
    }

    // The background reload is there at all (not about this request).
    bool backgroundPossible()
    {
        return sBackgroundReload && builder() && LLRHI::sVulkanGL && LLStartUp::getStartupState() >= STATE_STARTED &&
               !LLApp::isExiting() && !LLAppViewer::instance()->logoutRequestSent();
    }

    // This request may be made in the background (research/250 section 3.1).
    // Over the budget, a reload made at once (no frame allocating meanwhile)
    // keeps the peak lower.
    bool backgroundAllowed()
    {
        return backgroundPossible() && atSafePoint() && gPipeline.isInit() && gPipeline.shadersLoaded() &&
               !LLGLSLShader::sInstances.empty() && sBg.live_known && currentStructure() == sBg.live && !overBudget();
    }

    // The plan and the commit run setShaders: not while it would return at
    // once, nor in a batch or while the photo tools take a picture.
    bool canRunNow()
    {
        return atSafePoint() && gPipeline.isInit() && gPipeline.shadersLoaded() && LLViewerShaderMgr::sInitialized &&
               !LLViewerShaderMgr::sSkipReload && !sBatchDepth && !plus::presets::snapshotInProgress();
    }

    // Nothing on its way any more (the notice goes; tick() ends at its next call).
    void becomeIdle()
    {
        sBg.phase = Phase::Idle;
        notice::hide();
    }

    // A reload made now (the usual way), or the end of the session: what
    // waits or builds in the background is no longer needed.
    void stopBackground(const char* why)
    {
        if (sBg.phase == Phase::Idle)
        {
            return;
        }
        if (sBg.phase != Phase::Failed)
        {
            LL_INFOS("Plus") << "Background shader reload dropped: " << why << LL_ENDL;
        }
        if (LLRHI::ShaderReloadBuilder* b = builder())
        {
            b->cancel();
        }
        becomeIdle();
    }

    // A reload made the usual way while it lasts: setShaders (and each pass
    // of a parallel reload, which come back to beforeReload) is not sent to
    // the background.
    struct MadeNow
    {
        MadeNow() { ++sBg.made_now; }
        ~MadeNow() { --sBg.made_now; }
        MadeNow(const MadeNow&) = delete;
        MadeNow& operator=(const MadeNow&) = delete;
    };

    // The fallback when the plan cannot be made or a program did not build in
    // the background: the reload, made now the usual way (which may make a
    // program at a lower shader level). Only when that gives up on a program
    // too, the notice says so for a while (the state is the new settings'
    // programs, but for those).
    void reloadNow()
    {
        becomeIdle();
        const U32 given_up = LLGLSLShader::sGivenUp;
        {
            MadeNow made_now;
            LLPipeline::refreshCachedSettings();
            setShaders();
        }
        if (LLGLSLShader::sGivenUp != given_up)
        {
            LL_WARNS("Plus") << "Shader reload made the usual way: " << (LLGLSLShader::sGivenUp - given_up)
                             << " programs could not be made at any shader level (see the ShaderLoading lines above)" << LL_ENDL;
            sBg.phase = Phase::Failed;
            sBg.since.reset();
            notice::show(notice::Kind::Failed);
        }
    }

    void plan()
    {
        LLRHI::ShaderReloadBuilder* b = builder();
        b->begin();
        LLGLSLShader::Plan plan;
        LLTimer timer;
        {
            struct Planning
            {
                explicit Planning(LLGLSLShader::Plan& p) { LLGLSLShader::sPlan = &p; }
                ~Planning() { LLGLSLShader::sPlan = nullptr; }
                Planning(const Planning&) = delete;
                Planning& operator=(const Planning&) = delete;
            } planning(plan);
            setShaders();
        }
        b->planned();
        sBg.plan_seconds = timer.getElapsedTimeF64();
        if (plan.failed || !plan.programs)
        {
            LL_WARNS("Plus") << "Background shader reload: the plan pass handed " << plan.programs << " programs over"
                             << (plan.failed ? " and not all of them" : "") << ": the shaders are made now, the usual way" << LL_ENDL;
            b->cancel();
            reloadNow();
            return;
        }
        LLRHI::ShaderReloadProgress progress;
        b->progress(progress);
        LL_INFOS("Plus") << "Background shader reload: plan pass in " << sBg.plan_seconds << " s, " << plan.programs << " programs to "
                         << progress.threads << " build threads (low priority); the frames go on with the shaders in use" << LL_ENDL;
        sBg.phase = Phase::Building;
        sBg.build_frame = LLFrameTimer::getFrameCount();
    }

    // The last build is done: the commit at the next idle, or the failure.
    void built(const LLRHI::ShaderReloadProgress& progress)
    {
        LL_INFOS("Plus") << "Background shader build: " << progress.planned << " programs in " << progress.seconds << " s on "
                         << progress.threads << " threads while " << (LLFrameTimer::getFrameCount() - sBg.build_frame)
                         << " frames were drawn: " << progress.compiled << " compiled (" << progress.compile_ms / 1000.0
                         << " s of compiling), " << progress.cache_hits << " from the cache, " << progress.bundle_hits
                         << " from the bundle; " << progress.failed << " did not build" << LL_ENDL;
        if (progress.thrown)
        {
            // A build that threw would throw again made the usual way, on the main thread (research/275 L062):
            // the programs drawn with stay as they are (a target gone reads the backend's stand-in) until the
            // next change, and the notice says so for a while.
            LL_WARNS("Plus") << "Background shader reload: " << progress.thrown << " of " << progress.failed
                             << " programs that did not build threw (" << builder()->failures()
                             << "): not made the usual way; the programs in use stay" << LL_ENDL;
            builder()->cancel();
            sBg.phase = Phase::Failed;
            sBg.since.reset();
            notice::show(notice::Kind::Failed);
            return;
        }
        if (progress.failed)
        {
            // The targets and the cached settings are the new ones already: the programs follow now, made the
            // usual way (research/250 section 2.5), not drawn with as they are until the next change.
            LL_WARNS("Plus") << "Background shader reload: " << progress.failed << " programs did not build (" << builder()->failures()
                             << "): the shaders are made now, the usual way" << LL_ENDL;
            builder()->cancel();
            reloadNow();
            return;
        }
        sBg.phase = Phase::Ready;
        notice::show(notice::Kind::Switching, 1.f);
    }

    void commit()
    {
        LLRHI::ShaderReloadBuilder* b = builder();
        LLTimer timer;
        {
            struct Committing
            {
                LLRHI::ShaderReloadBuilder* mBuilder;
                explicit Committing(LLRHI::ShaderReloadBuilder* builder) : mBuilder(builder)
                {
                    sBg.committing = true;
                    sBg.commit_ran = false;
                    mBuilder->commitBegin();
                }
                ~Committing()
                {
                    mBuilder->commitEnd(sBg.commit_ran);
                    sBg.committing = false;
                }
                Committing(const Committing&) = delete;
                Committing& operator=(const Committing&) = delete;
            } committing(b);
            LLPipeline::refreshCachedSettings();    // as a handler before its reload, with the values as they are now
            setShaders();
        }
        if (!sBg.commit_ran)
        {
            return;     // setShaders returned at once: the builds went (commitEnd); planned again at the next idle
        }
        LL_INFOS("Plus") << "Background shader reload: committed in " << timer.getElapsedTimeF64() << " s (the plan pass took "
                         << sBg.plan_seconds << " s; " << sBg.whole.getElapsedTimeF64() << " s from the first of " << sBg.requests
                         << " requests)" << LL_ENDL;
        becomeIdle();
    }

    // Each idle while there is work (doOnIdleRepeating: true ends it).
    bool tick()
    {
        if (!backgroundPossible())
        {
            stopBackground("the session ends");
        }
        switch (sBg.phase)
        {
        case Phase::Idle:
            break;
        case Phase::Settling:
            if (sBg.since.getElapsedTimeF32() >= SETTLE_SECONDS && canRunNow())
            {
                plan();
            }
            break;
        case Phase::Building:
        {
            LLRHI::ShaderReloadProgress progress;
            builder()->progress(progress);
            if (progress.built >= progress.planned)
            {
                if (!progress.failed || canRunNow())   // a failure reloads now: not in a picture or a batch
                {
                    built(progress);
                }
            }
            else
            {
                notice::show(notice::Kind::Preparing, (F32)progress.built / (F32)llmax(1u, progress.planned));
            }
            break;
        }
        case Phase::Ready:
            if (canRunNow())
            {
                commit();
                if (sBg.phase == Phase::Ready)
                {
                    sBg.phase = Phase::Settling;    // the commit did not run: plan again (the builds are gone)
                    sBg.since.reset();
                }
            }
            break;
        case Phase::Failed:
            if (sBg.since.getElapsedTimeF32() >= FAILURE_SECONDS)
            {
                becomeIdle();
            }
            break;
        }
        if (sBg.phase == Phase::Idle)
        {
            sBg.ticking = false;
            return true;
        }
        return false;
    }

    // The settings are new and the programs drawn with are not: the render
    // targets follow the settings at once (as the reload's own release and
    // create did right after the handler), so no pass draws into a target the
    // settings no longer make, or misses one they now need (the sun's maps
    // when shadows go from 0 to 1). A program drawn with that reads a target
    // gone gets the backend's stand-in. The handlers that remake them right
    // before their reload (reflections, mirrors, SSR, transparent water)
    // leave it to this (remakeGLBuffersForReload in llviewercontrol.cpp).
    void request()
    {
        LLPipeline::refreshCachedSettings();    // as handleSetShaderChanged: the targets below follow every value
        remakeTargets();
        if (sBg.phase == Phase::Building || sBg.phase == Phase::Ready)
        {
            LL_INFOS("Plus") << "Background shader reload: asked for again while building: that plan is dropped, the last "
                             << "request is planned" << LL_ENDL;
            builder()->cancel();
        }
        if (sBg.phase == Phase::Idle || sBg.phase == Phase::Failed)
        {
            sBg.whole.reset();
            sBg.requests = 0;
            LL_INFOS("Plus") << "Background shader reload asked for: the frames go on with the shaders in use until it is done"
                             << LL_ENDL;
        }
        ++sBg.requests;
        sBg.phase = Phase::Settling;
        sBg.since.reset();
        notice::show(notice::Kind::Preparing, 0.f);
        if (!sBg.ticking)
        {
            sBg.ticking = true;
            doOnIdleRepeating(&tick);
        }
    }
}

    Batch::Batch()
    {
        ++sBatchDepth;
    }

    Batch::~Batch()
    {
        if (--sBatchDepth)
        {
            return;
        }
        if (std::uncaught_exceptions())
        {
            doOnIdleOneTime(&flush);    // not from an unwinding destructor; still made, at the next idle
            return;
        }
        flush();
    }

    bool deferBuffers()
    {
        if (!sBatchDepth)
        {
            return false;
        }
        ++sRemakesAsked;
        return true;
    }

    bool beforeReload()
    {
        if (sBatchDepth)
        {
            ++sReloadsAsked;
            return true;
        }
        if (LLGLSLShader::sPlan)
        {
            return false;   // research/250: the plan pass itself
        }
        if (sBg.committing)
        {
            // research/250: the commit, one plain pass whose links take the background builds.
            sBg.commit_ran = true;
            noteLive();
            LLRHI::freeDeferred();
            return false;
        }
        if (!atSafePoint())
        {
            stopBackground("a reload is made now (in a snapshot)");
            noteLive();     // the first load before login too: what the programs are made with
            return false;
        }
        if (!sBg.made_now && backgroundAllowed())
        {
            request();
            return true;
        }
        stopBackground("a reload is made now");
        noteLive();
        // Inside a pass of a parallel reload (this one's, or a caller's such
        // as the shader bake), the call returns false: that pass goes on below.
        if (sParallelSetShaders && LLRHI::sParallelShaderReload)
        {
            MadeNow made_now;
            if (LLRHI::sParallelShaderReload(&setShaders))
            {
                return true;
            }
        }
        LLRHI::freeDeferred();
        return false;
    }

    void afterBuffersReleased()
    {
        if (atSafePoint() && !LLGLSLShader::sPlan)
        {
            LLRHI::freeDeferred();
        }
    }

    bool planning()
    {
        return LLGLSLShader::sPlan != nullptr;
    }

    bool pending()
    {
        return sBg.phase == Phase::Settling || sBg.phase == Phase::Building || sBg.phase == Phase::Ready;
    }

    bool inBackground()
    {
        // Not while reloads are held back (a quality level or an import going in): the reload at
        // their end may be made otherwise, and what a handler made meanwhile must be there then.
        return !sBatchDepth && !LLViewerShaderMgr::sSkipReload && backgroundAllowed();
    }

    bool skipHeroWorkaround()
    {
        if (!LLRHI::sVulkanGL)
        {
            return false;
        }
        // The workaround clears GL's program binary cache (no such folder with
        // Vulkan: "Invalid path") and reloads, from inside the frame's draws.
        // Its own comment calls it a fix that works "for some reason". Turning
        // mirrors on already reloads (handleReflectionProbeDetailChanged), and
        // the programs are only made again from the same sources: the same
        // SPIR-V. Should the programs lack the mirrors' define all the same,
        // the reload comes at the next idle instead, outside the draws (none
        // when one is on its way: research/250, it plans with the settings).
        const auto& defines = LLGLSLShader::sGlobalDefines;
        if (defines.find("HERO_PROBES") == defines.end() && !pending())
        {
            LL_INFOS("Plus") << "Mirrors on with programs made without them: shaders reload at the next idle" << LL_ENDL;
            doOnIdleOneTime(&setShaders);
        }
        return true;
    }

    // ------------------------------------------------------------------
    // research/250: PlanScope

    struct PlanScope::State
    {
        // A shader drawn with, as the plan found it.
        struct Settings
        {
            LLGLSLShader* shader = nullptr;
            std::vector<std::pair<std::string, GLenum>> files;
            LLGLSLShader::defines_map_t defines;
            LLShaderFeatures features;
            std::string name;
            S32 level = 0;
            S32 group = 0;
            LLGLSLShader* rigged = nullptr;
            size_t gltf_variants = 0;
        };
        std::vector<Settings> shaders;
        std::unordered_set<LLGLSLShader*> kept;
        std::vector<LLGLSLShader*>* shader_list = nullptr;  // LLViewerShaderMgr's (private: given by setShaders)
        std::vector<LLGLSLShader*> list_kept;
        std::map<std::string, GLuint> vertex_objects;
        std::map<std::string, GLuint> fragment_objects;
        LLGLSLShader::defines_map_t global_defines;
        std::vector<S32> levels;
        S32 max_avatar_level = 0;
        bool glow = false;
        bool cloth = false;
        bool transparent_water = false;
        bool shaders_loaded = false;

        // Each shader drawn with starts the pass from nothing, as unloadShaders() leaves it: the
        // settings setShaders adds to (addPermutation, the features) would pile up otherwise.
        void keepShaders()
        {
            shaders.reserve(LLGLSLShader::sInstances.size());
            for (LLGLSLShader* shader : LLGLSLShader::sInstances)
            {
                Settings& settings = shaders.emplace_back();
                settings.shader = shader;
                settings.files.swap(shader->mShaderFiles);
                settings.defines.swap(shader->mDefines);
                settings.features = shader->mFeatures;
                shader->mFeatures = LLShaderFeatures();
                settings.name = shader->mName;
                settings.level = shader->mShaderLevel;
                settings.group = shader->mShaderGroup;
                settings.rigged = shader->mRiggedVariant;
                settings.gltf_variants = shader->mGLTFVariants.size();
                kept.insert(shader);
            }
        }

        // A shader the pass planned that is not drawn with goes back to nothing, as unload() leaves
        // it (first: one may be a GLTF variant the pass added, which goes below); the shaders drawn
        // with, as they were.
        void putBackShaders(const LLGLSLShader::Plan* plan)
        {
            if (plan)
            {
                for (LLGLSLShader* shader : plan->touched)
                {
                    if (!kept.count(shader))
                    {
                        shader->mShaderFiles.clear();
                        shader->mDefines.clear();
                        shader->mFeatures = LLShaderFeatures();
                    }
                }
            }
            for (Settings& settings : shaders)
            {
                LLGLSLShader* shader = settings.shader;
                shader->mShaderFiles = std::move(settings.files);
                shader->mDefines = std::move(settings.defines);
                shader->mFeatures = settings.features;
                shader->mName = std::move(settings.name);
                shader->mShaderLevel = settings.level;
                shader->mShaderGroup = settings.group;
                shader->mRiggedVariant = settings.rigged;
                if (shader->mGLTFVariants.size() > settings.gltf_variants)
                {
                    shader->mGLTFVariants.resize(settings.gltf_variants);   // variants the pass added, never made
                }
            }
        }

        // The pass loads the feature libraries again (with its defines) into lists of its own.
        void setLibrariesAside(LLViewerShaderMgr& mgr)
        {
            vertex_objects.swap(mgr.mVertexShaderObjects);
            fragment_objects.swap(mgr.mFragmentShaderObjects);
        }

        // The shader objects the pass loaded (its libraries and the programs' files), attached to
        // no program now, go; the lists of the shaders drawn with come back.
        void putBackLibraries(LLViewerShaderMgr& mgr, const LLGLSLShader::Plan* plan)
        {
            std::vector<GLuint> objects;
            if (plan)
            {
                objects = plan->objects;
            }
            for (const auto& entry : mgr.mVertexShaderObjects)
            {
                objects.push_back(entry.second);
            }
            for (const auto& entry : mgr.mFragmentShaderObjects)
            {
                objects.push_back(entry.second);
            }
            std::sort(objects.begin(), objects.end());
            objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
            for (GLuint object : objects)
            {
                if (object && glIsShader(object))
                {
                    glDeleteShader(object);
                }
            }
            mgr.mVertexShaderObjects = std::move(vertex_objects);
            mgr.mFragmentShaderObjects = std::move(fragment_objects);
        }

        // What setShaders sets before and between the programs (the shader list: some programs are added
        // to it as they are made, not only at its end).
        void keepGlobals(const LLViewerShaderMgr& mgr, std::vector<LLGLSLShader*>& list)
        {
            shader_list = &list;
            list_kept.swap(list);
            global_defines = LLGLSLShader::sGlobalDefines;
            levels = mgr.mShaderLevel;
            max_avatar_level = mgr.mMaxAvatarShaderLevel;
            glow = LLPipeline::sRenderGlow;
            cloth = LLPipeline::RenderAvatarCloth;
            transparent_water = LLPipeline::sRenderTransparentWater;
            shaders_loaded = gPipeline.mShadersLoaded;
        }

        void putBackGlobals(LLViewerShaderMgr& mgr)
        {
            *shader_list = std::move(list_kept);
            LLGLSLShader::sGlobalDefines = std::move(global_defines);
            mgr.mShaderLevel = std::move(levels);
            mgr.mMaxAvatarShaderLevel = max_avatar_level;
            LLPipeline::sRenderGlow = glow;
            LLPipeline::RenderAvatarCloth = cloth;
            LLPipeline::sRenderTransparentWater = transparent_water;
            gPipeline.mShadersLoaded = shaders_loaded;
        }
    };

    PlanScope::PlanScope(bool planning, std::vector<LLGLSLShader*>& shader_list)
    {
        if (!planning)
        {
            return;
        }
        mState = std::make_unique<State>();
        LLViewerShaderMgr& mgr = *LLViewerShaderMgr::instance();
        mState->keepShaders();
        mState->setLibrariesAside(mgr);
        mState->keepGlobals(mgr, shader_list);
    }

    PlanScope::~PlanScope()
    {
        if (!mState)
        {
            return;
        }
        LLViewerShaderMgr& mgr = *LLViewerShaderMgr::instance();
        const LLGLSLShader::Plan* plan = LLGLSLShader::sPlan;
        mState->putBackLibraries(mgr, plan);
        mState->putBackShaders(plan);
        mState->putBackGlobals(mgr);
    }
} // namespace plus::shaderreload
