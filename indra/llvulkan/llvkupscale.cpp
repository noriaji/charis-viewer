/**
 * @file llvkupscale.cpp
 * @brief Upscaling of the main view (research/165): AMD FidelityFX Super
 *        Resolution 1 (EASU, RCAS) and a bilinear stretch recorded directly
 *        in Vulkan.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 *
 * The EASU and RCAS shaders below are a GLSL port of AMD FidelityFX Super
 * Resolution 1.0 (ffx_fsr1.h, the FsrEasuCon / FsrEasuF and FsrRcasCon /
 * FsrRcasF functions):
 *
 *   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "linden_common.h"

#include "llvkupscale.h"

#include "llrhiupscale.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cmath>
#include <mutex>
#include <string>

namespace
{
    // One source, three passes (KIND 0 EASU, 1 bilinear, 2 RCAS: LLRHI::UpscalePass::Kind).
    // Pixel rows run as the target's: gl_FragCoord and the source's texture
    // coordinates both count from the first row in memory, as the other
    // native passes read their sources (llvklook.cpp).
    const char* const UPSCALE_FRAGMENT = R"GLSL(
layout(std140, set = 0, binding = 0) uniform Upscale
{
    vec4 con0;
    vec4 con1;
    vec4 con2;
    vec4 con3;
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

#if KIND == 0
// ---- EASU: FsrEasuF of ffx_fsr1.h ----
// con0.xy input / output size, con0.zw 0.5 * that - 0.5 (the input pixel of
// an output pixel's centre); con1..con3 the 12-tap pattern's gather points
// (FsrEasuCon), in texture coordinates from the top left of texel F:
//      b c
//    e f g h
//    i j k l
//      n o

// The direction and the length of the edge from one 2x2 quad's centre (FsrEasuSetF).
void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    // Horizontal: B C D, the ratio of the ends' difference to the larger step.
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 65536.0);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    // Vertical: A C E.
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 65536.0);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

// One tap of the windowed, stretched Lanczos-2 approximation (FsrEasuTapF).
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main()
{
    vec2 pp = floor(gl_FragCoord.xy) * u.con0.xy + u.con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 p0 = fp * u.con1.xy + u.con1.zw;
    vec2 p1 = p0 + u.con2.xy;
    vec2 p2 = p0 + u.con2.zw;
    vec2 p3 = p0 + u.con3.xy;
    // Gathers: x (left, lower row), y (right, lower row), z (right, upper row), w (left, upper row).
    vec4 bczzR = textureGather(source, p0, 0);
    vec4 bczzG = textureGather(source, p0, 1);
    vec4 bczzB = textureGather(source, p0, 2);
    vec4 ijfeR = textureGather(source, p1, 0);
    vec4 ijfeG = textureGather(source, p1, 1);
    vec4 ijfeB = textureGather(source, p1, 2);
    vec4 klhgR = textureGather(source, p2, 0);
    vec4 klhgG = textureGather(source, p2, 1);
    vec4 klhgB = textureGather(source, p2, 2);
    vec4 zzonR = textureGather(source, p3, 0);
    vec4 zzonG = textureGather(source, p3, 1);
    vec4 zzonB = textureGather(source, p3, 2);

    // Luma times 2.
    vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;

    // The edge's direction and length, from the four quads round the pixel, bilinearly weighted.
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    // Stretched along the edge: 1 on the axes, sqrt(2) on the diagonals.
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // The negative lobe: smaller along an edge.
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    // The ringing limit: the four nearest texels' range.
    vec3 f = vec3(ijfeR.z, ijfeG.z, ijfeB.z);
    vec3 g = vec3(klhgR.w, klhgG.w, klhgB.w);
    vec3 j = vec3(ijfeR.y, ijfeG.y, ijfeB.y);
    vec3 k = vec3(klhgR.x, klhgG.x, klhgB.x);
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));   // b
    easuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));   // c
    easuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));   // i
    easuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);                                 // j
    easuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);                                 // f
    easuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));   // e
    easuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);                                 // k
    easuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));   // l
    easuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));   // h
    easuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);                                 // g
    easuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));   // o
    easuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));   // n

    vec3 color = min(max4, max(min4, aC / aW));
    // Alpha as the source has it under the pixel's centre (1 / output size = con0.xy * con1.xy).
    float alpha = textureLod(source, (floor(gl_FragCoord.xy) + 0.5) * u.con0.xy * u.con1.xy, 0.0).a;
    frag_color = vec4(color, alpha);
}
#elif KIND == 1
// ---- Bilinear: the official RenderResolutionDivisor's stretch, read by hand
// (the sampler's filter is the GL texture's of the moment). con0 as EASU's,
// con1.xy the input size.
void main()
{
    vec2 pos = floor(gl_FragCoord.xy) * u.con0.xy + u.con0.zw;
    vec2 base = floor(pos);
    vec2 t = pos - base;
    ivec2 last = ivec2(u.con1.xy) - 1;
    ivec2 i0 = clamp(ivec2(base), ivec2(0), last);
    ivec2 i1 = clamp(ivec2(base) + 1, ivec2(0), last);
    vec4 a = texelFetch(source, i0, 0);
    vec4 b = texelFetch(source, ivec2(i1.x, i0.y), 0);
    vec4 c = texelFetch(source, ivec2(i0.x, i1.y), 0);
    vec4 d = texelFetch(source, i1, 0);
    frag_color = mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}
#else
// ---- RCAS: FsrRcasF of ffx_fsr1.h (with FSR_RCAS_DENOISE) ----
// con0.x the sharpness as a scale (FsrRcasCon: 2^-stops, 1 the strongest).
const float RCAS_LIMIT = 0.25 - (1.0 / 16.0);

vec3 rcasLoad(ivec2 p, ivec2 last)
{
    return clamp(texelFetch(source, clamp(p, ivec2(0), last), 0).rgb, 0.0, 1.0);
}

float rcasMax3(float a, float b, float c) { return max(a, max(b, c)); }
float rcasMin3(float a, float b, float c) { return min(a, min(b, c)); }

void main()
{
    //    b
    //  d e f
    //    h
    ivec2 sp = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(source, 0) - 1;
    vec4 center = texelFetch(source, clamp(sp, ivec2(0), last), 0);
    vec3 b = rcasLoad(sp + ivec2( 0, -1), last);
    vec3 d = rcasLoad(sp + ivec2(-1,  0), last);
    vec3 e = clamp(center.rgb, 0.0, 1.0);
    vec3 f = rcasLoad(sp + ivec2( 1,  0), last);
    vec3 h = rcasLoad(sp + ivec2( 0,  1), last);

    // Luma times 2.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

    // Noise detection: less sharpening where the centre stands out alone.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float range = max(rcasMax3(bL, dL, eL), max(fL, hL)) - min(rcasMin3(bL, dL, eL), min(fL, hL));
    nz = clamp(abs(nz) / max(range, 1.0 / 65536.0), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    // The ring's range, and the largest negative lobe that keeps the result in 0..1.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(rcasMax3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * u.con0.x;
    lobe *= nz;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 color = (lobe * (b + d + h + f) + e) * rcpL;
    frag_color = vec4(color, center.a);
}
#endif
)GLSL";

    // The uniform block Upscale, as std140 lays it out.
    struct Uniforms
    {
        F32 con0[4];
        F32 con1[4];
        F32 con2[4];
        F32 con3[4];
    };
    static_assert(sizeof(Uniforms) == 64, "Uniforms must match the std140 block Upscale");

    constexpr U32 KIND_COUNT = 3;
    const char* const PASS_NAMES[KIND_COUNT] = { "upscale easu", "upscale bilinear", "upscale rcas" };

    class Renderer
    {
    public:
        bool draw(const LLRHI::UpscalePass& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake()
        {
            bool all = true;
            for (U32 kind = 0; kind < KIND_COUNT; ++kind)
            {
                all = ready(kind) && all;
            }
            return all;
        }

    private:
        bool ready(U32 kind);

        bool mTried[KIND_COUNT] = {};
        bool mReady[KIND_COUNT] = {};
        std::string mSource[KIND_COUNT];
        LLVKFullscreen::Pass mPass[KIND_COUNT];
        bool mLogged[KIND_COUNT] = {};
        bool mWarnedTarget = false;     // a bound target of another size (warned once)
    };

    Renderer sRenderer;

    bool Renderer::ready(U32 kind)
    {
        if (!mTried[kind])
        {
            mTried[kind] = true;
            mSource[kind] = "#version 450\n#define KIND " + std::to_string(kind) + "\n" + UPSCALE_FRAGMENT;
            mReady[kind] = mPass[kind].init(PASS_NAMES[kind], mSource[kind].c_str(), 1);
            if (!mReady[kind])
            {
                LL_WARNS("VKUpscale") << "The " << PASS_NAMES[kind] << " pass could not be set up; the picture is stretched as it is" << LL_ENDL;
            }
        }
        return mReady[kind];
    }

    // FsrEasuCon of ffx_fsr1.h, the input viewport being the whole input.
    void easuConstants(Uniforms& u, F32 in_w, F32 in_h, F32 out_w, F32 out_h)
    {
        u.con0[0] = in_w / out_w;
        u.con0[1] = in_h / out_h;
        u.con0[2] = 0.5f * in_w / out_w - 0.5f;
        u.con0[3] = 0.5f * in_h / out_h - 0.5f;
        u.con1[0] = 1.f / in_w;
        u.con1[1] = 1.f / in_h;
        u.con1[2] = 1.f / in_w;
        u.con1[3] = -1.f / in_h;
        u.con2[0] = -1.f / in_w;
        u.con2[1] = 2.f / in_h;
        u.con2[2] = 1.f / in_w;
        u.con2[3] = 2.f / in_h;
        u.con3[0] = 0.f;
        u.con3[1] = 4.f / in_h;
        u.con3[2] = 0.f;
        u.con3[3] = 0.f;
    }

    bool Renderer::draw(const LLRHI::UpscalePass& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const U32 kind = pass.kind;
        if (kind >= KIND_COUNT || !pass.source_texture || !pass.source_width || !pass.source_height || !pass.width || !pass.height ||
            !ready(kind))
        {
            return false;
        }
        Uniforms uniforms = {};
        if (kind == LLRHI::UpscalePass::RCAS)
        {
            // FsrRcasCon: sharpness in stops, 0 the strongest; ours 1 is the strongest, 0 none (two stops: 0.25).
            const F32 amount = llclamp(pass.sharpness, 0.f, 1.f);
            uniforms.con0[0] = amount > 0.f ? std::exp2(-2.f * (1.f - amount)) : 0.f;
        }
        else
        {
            easuConstants(uniforms, (F32)pass.source_width, (F32)pass.source_height, (F32)pass.width, (F32)pass.height);
            if (kind == LLRHI::UpscalePass::BILINEAR)
            {
                uniforms.con1[0] = (F32)pass.source_width;
                uniforms.con1[1] = (F32)pass.source_height;
            }
        }

        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({ pass.source_texture }, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != pass.width || target.height != pass.height)
        {
            LLVKTracker::endPass();
            if (!mWarnedTarget)
            {
                mWarnedTarget = true;
                LL_WARNS("VKUpscale") << "Upscale: the bound target is " << target.width << "x" << target.height << ", not the "
                                      << pass.width << "x" << pass.height << " it was called for; not drawn" << LL_ENDL;
            }
            return false;
        }
        const LLVKFullscreen::Image images[1] = { { pass.source_texture } };
        const bool drawn = mPass[kind].draw(cmd, target, &uniforms, sizeof(uniforms), images, 1);
        LLVKTracker::endPass();
        if (drawn && !mLogged[kind])
        {
            mLogged[kind] = true;
            LL_INFOS("VKUpscale") << "Upscale: " << PASS_NAMES[kind] << " " << pass.source_width << "x" << pass.source_height << " to "
                                  << target.width << "x" << target.height << ", target format " << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool record_upscale(const LLRHI::UpscalePass& pass)
    {
        return sRenderer.draw(pass);
    }
}

void LLVKUpscale::install()
{
    // Installed whatever LLRHI_UPSCALE says: upscale.toggle may turn it on later.
    if (!LLRHI::sUpscale)
    {
        LL_INFOS("VKUpscale") << "Upscaling off (LLRHI_UPSCALE=0)" << LL_ENDL;
    }
    LLRHI::sUpscaleDraw = &record_upscale;
    LLVKShader::addBakeStep("upscale", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
