/**
 * @file ssr.h
 * @brief Screen space reflections in passes of their own: a port of the
 *        official project/graphics-care-package's (research/129).
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

#ifndef PLUS_SSR_H
#define PLUS_SSR_H

#include <map>
#include <string>

class LLGLSLShader;

// The hooks do nothing while the feature is off (PlusSSR 0, or
// RenderScreenSpaceReflections 0, the default), with GL or LLRHI_SSR=0, and for reflection
// probe faces and mirrors: the official screen space reflections stay. Main
// thread.
namespace plus::ssr
{
    // The <plus> hook in LLViewerShaderMgr::loadBasicShaders(): PLUS_SSR
    // along with the official SSR define, so reflectionProbeF.glsl can read
    // this feature's buffer (switched by a uniform: no shader rebuild).
    void addShaderDefines(std::map<std::string, std::string>& attribs);

    // The <plus> hook in LLPipeline::renderDeferredLighting(), after the
    // light map and before the lighting: the main view's reflections traced
    // into this feature's buffer.
    void render();

    // The <plus> hook at the end of LLPipeline::bindReflectionProbes(): on
    // the main view's lighting program, sceneMap is this frame's buffer
    // (plusSSRParams.x 1); every other program keeps the scene copy and the
    // official tracing (plusSSRParams.x 0).
    void bind(LLGLSLShader& shader);
}

#endif // PLUS_SSR_H
