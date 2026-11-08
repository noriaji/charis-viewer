/**
 * @file glare.h
 * @brief Glare (HDR bloom) and the sun's lens flare (research/108-glare.md).
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

#ifndef PLUS_GLARE_H
#define PLUS_GLARE_H

class LLGLSLShader;

namespace plus::glare
{
    // The <plus> hook in LLPipeline::renderFinalize(), after the exposure is
    // measured (generateExposure) and before tone mapping: adds the glare, the
    // lens flare and the cross filter to the scene's HDR target (mRT->screen).
    // Returns at once when all are off (PlusGlare, PlusLensFlare,
    // PlusCrossFilter, the defaults), the backend
    // cannot draw them (GL, or LLRHI_GLARE=0), or the snapshot is taken
    // without post-processing. Main thread.
    void render();

    // The <plus> hook in LLPipeline::renderFinalize()'s branch without HDR
    // (RenderHDREnabled off), where render() is not called: frees the glare's,
    // the lens flare's and the cross filter's targets (made again when HDR
    // comes back on). Main thread.
    void releaseWithoutHDR();

    // The <plus> hooks in LLPipeline::tonemap(), right after render(): with
    // PlusGlareInTonemap (off) render() leaves its composite to the tone
    // mapping program (postDeferredTonemap.glsl's <plus> part, research/125
    // #7). bindTonemap sets that part's uniforms on the bound program (to add
    // nothing when render() left nothing); unbindTonemap, after the draw,
    // frees the texture units it took. Main thread.
    void bindTonemap(LLGLSLShader* shader);
    void unbindTonemap();
}

#endif // PLUS_GLARE_H
