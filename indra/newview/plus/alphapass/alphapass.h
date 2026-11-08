/**
 * @file alphapass.h
 * @brief The alpha pool's glow pass without the batches whose faces have no
 *        glow, and the alpha pass's CPU counters (research/178 F0).
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

#ifndef PLUS_ALPHAPASS_ALPHAPASS_H
#define PLUS_ALPHAPASS_ALPHAPASS_H

#include "stdtypes.h"

#include <chrono>

class LLDrawInfo;
class LLGLSLShader;
class LLSpatialGroup;
class LLTextureEntry;

namespace plus::alphapass
{
    // LLDrawInfo::mRHIGlow: whether a face of the batch has glow.
    enum Glow : U8
    {
        GLOW_UNKNOWN = 0,   // not known (other partitions, or rewritten in place): drawn as upstream
        GLOW_NONE = 1,      // no face of the batch glows: its glow draw adds 0 to the alpha channel
        GLOW_SOME = 2,
    };

    // LLVolumeGeometryManager::registerFace: the face (its texture entry) went
    // into `info`, which it started when `new_info`. A batch's faces are
    // written into its vertex buffer in the same rebuild (genDrawInfo), with
    // the emissive attribute from the same entry's glow (LLFace::getGeometryVolume).
    void noteFace(LLDrawInfo& info, const LLTextureEntry* te, bool new_info);

    // LLVolumeGeometryManager::rebuildMesh: a face of the group is rewritten in
    // place (its batches stay). A glowing one makes the group's alpha batches
    // GLOW_UNKNOWN until its next full rebuild (its glow may be new, and its
    // emissive bytes run up to 3 vertices into the next face's). `marked`: the
    // group's batches were already marked in this rebuild.
    void noteMeshFace(LLSpatialGroup* group, const LLTextureEntry* te, bool& marked);

    // LLDrawPoolAlpha::renderAlpha / renderAlphaList, after a batch of the
    // post-water pool: whether it goes to its group's glow pass. Upstream: whenever its
    // vertex buffer has an emissive attribute, which the whole group's buffers
    // get when any face of the group glows (LLVolumeGeometryManager::rebuildGeom).
    // Here (LLRHI::sAlphaGlowSkip on) not when no face of the batch glows:
    // the emissive program's colour is the emissive alpha (0) times the
    // texture's alpha, blended (ZERO, ONE, ONE, ONE): it adds 0 and writes no
    // depth. Not in an OIT session (its fragments are nodes), not for GLTF
    // materials (their glow is the material's), never in depth passes. Counts.
    bool glowPass(LLSpatialGroup* group, const LLDrawInfo& draw, bool oit, bool depth_only);

    // The program changes in the alpha loop (bindDeferredShaderFast), before
    // the call (a program not bound fast yet takes the slow path). Counts.
    void noteBind(const LLGLSLShader& shader);

    // Around renderAlpha's (and renderAlphaList's) colour passes: their CPU time
    // and the AlphaPass line every 5 s, while LLRHI::sAlphaPassStats is on.
    class PassScope
    {
    public:
        explicit PassScope(bool depth_only);
        ~PassScope();
        PassScope(const PassScope&) = delete;
        PassScope& operator=(const PassScope&) = delete;
    private:
        bool mTiming = false;
        std::chrono::steady_clock::time_point mStart;
    };

    // Around a group's glow pass (after its batches; `drawing`: a list is not
    // empty). Its CPU time, and the groups whose glow pass was left out whole.
    class GlowScope
    {
    public:
        explicit GlowScope(bool drawing);
        ~GlowScope();
        GlowScope(const GlowScope&) = delete;
        GlowScope& operator=(const GlowScope&) = delete;
    private:
        bool mTiming = false;
        std::chrono::steady_clock::time_point mStart;
    };
}

#endif // PLUS_ALPHAPASS_ALPHAPASS_H
