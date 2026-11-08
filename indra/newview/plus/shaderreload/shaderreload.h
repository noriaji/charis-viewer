/**
 * @file shaderreload.h
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

#ifndef PLUS_SHADERRELOAD_H
#define PLUS_SHADERRELOAD_H

#include <memory>
#include <vector>

class LLGLSLShader;

// With GL (the GPU benchmark process) the hooks do nothing. Main thread.
namespace plus::shaderreload
{
    // The <plus> hook at the head of LLViewerShaderMgr::setShaders(), past
    // its own early returns. True: setShaders returns; the reload has run
    // already (in parallel: LLRHI::sParallelShaderReload ran setShaders
    // twice, each pass coming back here) or, research/250, it is made in the
    // background and the shaders drawn with stay until it is done. False:
    // setShaders goes on as usual, after the frames in flight have freed
    // what was deferred to them (LLRHI::freeDeferred); or it is the
    // background reload's plan pass or commit. All of it only after login,
    // outside snapshots. Inside a Batch (any time, GL too): true, the reload
    // is noted and made once at the batch's end.
    bool beforeReload();

    // research/250: the <plus> hook in setShaders right after its
    // releaseGLBuffers(): after login, outside snapshots, the frames in
    // flight end and the released targets are freed before the reload makes
    // the new ones (one set of targets at a time, not two).
    void afterBuffersReleased();

    // research/250: around the plan pass of setShaders (LLGLSLShader::sPlan
    // set). planning true: everything the pass sets besides the programs it
    // hands over (each shader's settings, the shader manager's lists and
    // levels, shader_list: its list of shaders for the sky's uniforms, the
    // global defines, the pipeline's flags setShaders writes) is kept here
    // and put back at the end of the scope, and the shader objects the pass
    // loaded are deleted. planning false: nothing.
    class PlanScope
    {
    public:
        PlanScope(bool planning, std::vector<LLGLSLShader*>& shader_list);
        ~PlanScope();
        PlanScope(const PlanScope&) = delete;
        PlanScope& operator=(const PlanScope&) = delete;

    private:
        struct State;
        std::unique_ptr<State> mState;
    };

    // research/250: true during the plan pass (features that note "the
    // programs have my define" in their addShaderDefines leave the note
    // alone then: the programs drawn with do not change).
    bool planning();

    // research/250: true from a background reload's request until its
    // commit (or its end): the programs drawn with are not yet those the
    // settings ask for. A feature that compares the two to decide whether
    // to ask for a reload asks anyway then (the request is merged).
    bool pending();

    // research/250: true when a reload asked for now would be made in the
    // background (the viewer does not stand still for it): plus/presets
    // then shows no "the viewer stands still" notice, and the handlers that
    // remake the render targets right before their reload leave that to it
    // (it makes them when asked for). False in a Batch and while reloads are
    // held back (LLViewerShaderMgr::sSkipReload).
    bool inBackground();

    // The <plus> hook in LLHeroProbeManager::update()'s workaround for the
    // official issue #3331 (a shader cache clear and a reload the first time
    // mirrors draw). True: skip it (Vulkan has neither the GL program cache
    // it clears nor a reason to reload: the programs already have
    // HERO_PROBES; if not, and no reload is pending, a reload is asked for
    // at the next idle, outside the frame's draws).
    bool skipHeroWorkaround();

    // gfxadv-close: settings changed together (the preferences' cancel puts
    // back every setting the floaters changed; the closing X of the advanced
    // graphics floater is a cancel) make the shaders and the render targets
    // once, at the end of the outermost Batch. Each such setting reloaded
    // every shader (2 to 7 s) and remade the targets by itself: six in a row
    // on the 2026-10-30 log, 25 s, the old targets piling up over the VRAM
    // budget. Main thread; nested batches flush at the outermost one's end.
    class Batch
    {
    public:
        Batch();
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
    };

    // The <plus> hook in the settings handlers that remake the render targets
    // (llviewercontrol.cpp). True: a Batch is open, the remake is noted and
    // comes once at its end (with the reload when there is one, which remakes
    // them too). False: remake them now, as before.
    bool deferBuffers();
}

#endif // PLUS_SHADERRELOAD_H
