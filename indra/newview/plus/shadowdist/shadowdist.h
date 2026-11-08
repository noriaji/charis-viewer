/**
 * @file shadowdist.h
 * @brief The shadow distance: one setting (PlusShadowDistance, near / normal /
 *        far / very far) for how far the sun's shadows reach and how sharp
 *        they stay there, part of the graphics presets and of the snapshot
 *        boost (research/107-shadow-distance.md in the sl-viewer repository).
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

#ifndef PLUS_SHADOWDIST_SHADOWDIST_H
#define PLUS_SHADOWDIST_SHADOWDIST_H

#include "stdtypes.h"
#include "v3math.h"
#include "v4math.h"

#include "glm/mat4x4.hpp"

#include <vector>
#include "llrhi.h"

class LLCamera;
class LLRenderTarget;

// The <plus> hooks in pipeline.cpp. Each gets the official value and returns
// it unchanged for reflection probes and mirrors (gCubeSnapshot) and while
// the setting is -1 (official behaviour, for comparisons). Main thread.
namespace plus::shadowdist
{
    // LLPipeline::generateSunShadow(): the farthest the sun's shadows reach
    // (m), in place of the official upper limit of the last cascade (512).
    // The draw distance still limits it.
    F32 maxDistance(F32 official_max);

    // LLPipeline::generateSunShadow(), after the splits (clip, the shader's
    // shadow_clip) are made: with the shadow focus on the subject
    // (PlusShadowFocus, and in snapshots), they are fitted to the camera's
    // focus (the avatar, or what the camera was pointed at), so the cascade
    // that holds the subject and its shadow ends right past them. Only when
    // that cascade ends nearer than in the distance's splits.
    void fitSplits(LLVector4& clip, F32 near_clip, F32 far_clip, const LLCamera& camera, const LLVector3& light_dir);

    // LLPipeline::generateSunShadow(), cascade j, once the official view and
    // projection are made: with PlusShadowStabilize (on), replaces them with a
    // stable orthographic cascade around the same receivers (points, agent
    // space) whose texel grid does not slide or turn with the camera (research/107
    // section 38). With PlusShadowShift or PlusShadowZRound, or the static sun
    // shadow cache on (the z hold, research/178 C1), and the shadow detail on,
    // its depth range is kept in steps too (research/151 2.3).
    // False (unchanged) when off, for probes and mirrors.
    bool stabilizeCascade(S32 j, const std::vector<LLVector3>& points, const LLVector3& light_dir, const LLCamera& camera,
                          U32 map_width, U32 map_height, glm::mat4& view, glm::mat4& proj);

    // LLPipeline::reuseShadowCascade() (the static shadow cache): true when both
    // are stable cascades of the same turn and size, so the cached one's texels
    // are the new one's wherever its centre is (it only has to hold the receivers).
    bool sameStableGrid(const glm::mat4& view_a, const glm::mat4& proj_a, const glm::mat4& view_b, const glm::mat4& proj_b);

    // For the static shadow cache's log (research/178 C1): whether stable
    // cascades keep their depth range in steps now (PlusShadowShift,
    // PlusShadowZRound, or the z hold with the static sun shadow cache on, with
    // the shadow detail on), and whether the z hold is on (LLRHI_SHADOW_Z_HOLD,
    // on unless 0; shadowzhold.toggle with LLRHI_CUE_DIR flips it).
    bool depthStepsActive();
    bool zHoldOn();

    // LLPipeline::generateSunShadow(): RenderShadowSplitExponent, how the four
    // cascades share that distance (higher: more of the detail near the eye).
    LLVector3 splitExponent(const LLVector3& official);

    // LLPipeline::allocateShadowBuffer(): the scale of the four sun shadow maps
    // to res_x x res_y (the official one, RenderShadowResolutionScale, times
    // this level's or the snapshot boost's, within a share of the VRAM).
    F32 sunResolutionScale(F32 official_scale, U32 res_x, U32 res_y);

    // LLPipeline::generateSunShadow() and the sun cache entry: the direction
    // (to the sun or moon) the sun shadows are made with. While the light
    // drifts with the day cycle, the stable cascades' texel grid would turn
    // with it every frame and the shadows shimmer; this holds the direction
    // and takes the live one only when it is PlusShadowLightStep degrees away
    // (or the body changed). Probes and mirrors, and a step of 0: the live one.
    LLVector3 shadowCasterDir(const LLVector3& live, bool sun_up);

    // LLPipeline::bindDeferredShader() and the shadow receiver cull: the factor
    // on the sun shadow bias. The official bias is a depth in the map's own
    // units, worth four to nine texels: finer maps take a smaller one. While
    // the shadow detail (PlusShadowPrecision) is on it is 0: samplingProjection()
    // moves the receivers instead.
    F32 biasScale();

    // LLPipeline::generateSunShadow(), cascade j: the projection the shaders
    // sample it with (in mSunShadowMatrix, in place of proj): proj with the
    // receivers moved towards the light by the bias as a length, in step with
    // this cascade's texels and the sun's height and never longer than the
    // official one. near_depth..far_depth: the cascade's split (eye depths,
    // before the shader's 0.75 / 1.25 bands). proj itself while the detail is
    // official.
    glm::mat4 samplingProjection(S32 j, const glm::mat4& proj, const glm::mat4& view, const LLCamera& camera,
                                 F32 near_depth, F32 far_depth, const LLVector3& light_dir, const LLRenderTarget& map);

    // LLPipeline::renderShadowCasters(): held while the casters of sun cascade
    // `cascade` (0-3; anything else: none) are drawn, it gives them a
    // slope-scaled depth bias (LLRHI::sShadowDepthBias, Vulkan) while the
    // shadow detail is on: surfaces the light grazes (the ground) are pushed
    // back in the map instead of every receiver being moved towards the light,
    // so a shadow starts right at what casts it. Not for probes and mirrors.
    class CasterBiasScope
    {
    public:
        explicit CasterBiasScope(S32 cascade);
        ~CasterBiasScope();

        CasterBiasScope(const CasterBiasScope&) = delete;
        CasterBiasScope& operator=(const CasterBiasScope&) = delete;

    private:
        LLRHI::ShadowDepthBias mSaved;
    };

    // The casters' depth bias clamp sun cascade `cascade` was last given by
    // samplingProjection() ([0, 1] depth; 0 while their bias is off), for
    // plus/shadowshift (a cached depth moved by whole texels was drawn with it).
    F32 casterClamp(S32 cascade);

    // LLPipeline::generateSunShadow(), once a frame: true when the casters'
    // bias was turned on or off since the last call (the static shadow cache's
    // depth was drawn with the other one).
    bool casterBiasChanged();

    // The shadow receiver cull (research/64): the most shadowUtil.glsl's normal
    // offset moves a receiver (m), on top of the official offset.
    F32 receiverNormalOffsetMax();

    // LLPipeline::bindDeferredShader(): shadowUtil.glsl's normal offset in texels
    // (plus_normal_offset_texels): 1.5, or 0 with PlusShadowNormalOffset off.
    F32 normalOffsetTexels();

    // LLPipeline::renderSunShadowCascades(), the main view's, at its end: what
    // the shadow receiver cull did with your avatar's rigged casters (per
    // cascade position k: index[k] the cascade, tested[k] and kept[k] its
    // groups). With PlusShadowDiagnostics, logs a line when your avatar's
    // casters were left out of a cascade they were in last pass, skinned
    // casters were not drawn, or a foot is outside the box the culls trust
    // (research/107 section 27).
    void noteAgentCasters(const S32* index, const U32* tested, const U32* kept);

    // LLPipeline::renderDeferredLighting(), the screen-space blur of the sun
    // light map, from the shadow softness (PlusShadowSoftness, the user's
    // choice, snapshots too): RenderShadowGaussian (only x, the shadow's
    // sigma; ambient occlusion keeps y), RenderShadowBlurSize, and
    // RenderShadowBlurDistFactor for that blur_size. The official factor 0
    // blurs the same number of pixels at every distance, wider than a thin
    // shadow far away; the factor returned holds the blur to the softness's
    // reach in metres.
    LLVector3 blurGaussian(const LLVector3& official);
    F32 blurSize(F32 official);
    F32 blurDistFactor(F32 official, F32 blur_size);
}

#endif // PLUS_SHADOWDIST_SHADOWDIST_H
