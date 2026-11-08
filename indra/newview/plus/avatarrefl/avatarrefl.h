/**
 * @file avatarrefl.h
 * @brief Avatars in reflections (research/127): stage 0, avatars always in
 *        mirrors; stage 1, your avatar mirrored in the floor or the water
 *        under it (a planar reflection of its own, every GPU, no ray tracing).
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

#ifndef PLUS_AVATARREFL_H
#define PLUS_AVATARREFL_H

#include <map>
#include <string>

class LLGLSLShader;

// The hooks below are the <plus> lines in official files. With both settings
// off (the default) each is a flag test, and the shaders are the official ones.
// They also carry the water mirror (watermirror.h, PlusWaterMirror).
namespace plus::avatarrefl
{
    // In LLHeroProbeManager::renderProbes(): whether the mirror's faces draw
    // avatars (and particles) although its maker did not mark it dynamic
    // (PlusMirrorAvatars).
    bool mirrorAvatars();

    // In LLViewerShaderMgr::loadBasicShaders(), with the global defines:
    // PLUS_AVATAR_REFL while the planar reflection is on (PlusAvatarReflection,
    // Vulkan only). class3/deferred/reflectionProbeF.glsl then samples it.
    void addShaderDefines(std::map<std::string, std::string>& attribs);

    // In display(), right after the mirrors and before the main view: picks
    // the plane under your avatar (the floor it stands on, the terrain, or the
    // water) and draws the avatars from your eye mirrored in it into a target
    // of their own.
    void renderPlanar();

    // At the end of LLPipeline::bindReflectionProbes(): the reflection's two
    // textures and its uniforms, for a program built with PLUS_AVATAR_REFL.
    // Outside the main view (probe faces, mirrors, this reflection's own
    // pass, previews) it is turned off for the program.
    void bind(LLGLSLShader& shader);

    // For plus/ssr's trace (the new screen space reflections, main view):
    // while your reflection is drawn into the main view, your avatar's box
    // (from the scene copy's eye space: inside -1..1 on each axis), the plane
    // in this frame's eye space and its weight, thickness and least normal
    // match (as bind() gives the opaque lighting). The trace drops its hits in
    // the box on the plane, as screenSpaceReflUtil.glsl does for the official
    // rays (research/210 A2). false (nothing written) while it is not drawn.
    bool selfSkip(float (&box)[16], float (&plane)[4], float (&params)[4]);

    // While it lives, one of our planar passes (this reflection, the water
    // mirror) is drawn under a probe face's rules (gCubeSnapshot set) as a
    // radiance pass, as LLHeroProbeManager::update() draws a mirror. Otherwise
    // the probe manager's pass, left from its last update (it runs after
    // display()), would turn every few frames into an irradiance pass: no
    // ambient light, the sky's blues taken out, no clouds (fix-round32: the
    // sea blinked under a personal sky). Both are put back as they
    // were on every way out of the scope.
    class CubeSnapshotScope
    {
    public:
        CubeSnapshotScope();
        ~CubeSnapshotScope();
        CubeSnapshotScope(const CubeSnapshotScope&) = delete;
        CubeSnapshotScope& operator=(const CubeSnapshotScope&) = delete;

    private:
        const bool mSavedCubeSnapshot;
        const bool mSavedRadiancePass;
    };
}

#endif // PLUS_AVATARREFL_H
