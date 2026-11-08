/**
 * @file llvkpass.cpp
 * @brief The Vulkan backend's side of the full-screen passes (research/137 N12):
 *        each pass's barrier plan, the target as the viewer named it, the counts.
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

#include "linden_common.h"

#include "llvkpass.h"

#include "llrhipass.h"
// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (this file has no PCH; research/137 N7's finding).
#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"

#if LL_GL_FUNC_POINTER

#include <algorithm>

// A pass (LLRHI::PassDesc) is opened by the render thread right after it
// bound the pass's target, and closed after the pass's last draw (usually
// after the target's flush). Between the two the viewer draws exactly as it
// would without the pass: this file records nothing but, with
// LLRHI_NATIVE_PASSES=1, one barrier batch at the pass's start.
//
// The plan at the start (sNativePasses 1) moves, in one batch:
//   - each named input that is not in shader-read layout to it, as the draw
//     that samples it would (transition(), the same as direct_draw's
//     barriers: the layout the texture's tracking says, the frame's main
//     commands, the rendering scope closed first);
//   - with LLRHI_NATIVE_TARGETS=1, the colour attachments of the named target
//     to attachment layout, as needs_attachment_layouts and
//     to_attachment_layouts would before the first draw or clear.
// Left to the draws, as before: the target's depth (whether it is attached
// read-only depends on what each draw samples), an input that is one of the
// bound target's attachments (sampled while attached), names the backend
// does not know or that have no image yet, and everything while the frame is
// not recorded, a probe list capture keeps the draws for later, a depth-only
// pass, an occlusion query, the census, a translation that is suppressed, or
// an OIT session is open (those change where or whether draws record).
// So the commands differ from the passes off only in where in the frame's
// main commands these layout changes stand: before the pass's first draw
// instead of at it (nothing between the two reads or writes those images:
// the pass's own state calls only, and its target's clear, which writes the
// target's attachments, never an input).
namespace
{
    using namespace llvkgl;

    constexpr U32 NAMES = 64;      // pass names kept apart (32 in pipeline.cpp now; more: counted under "others")

    struct Counts
    {
        U32 passes = 0;             // opened
        U32 replaced = 0;           // opened while another was open (that one closed first: the next pass of a step)
        U32 not_planned = 0;        // with LLRHI_NATIVE_PASSES=1, passes the plan left to the draws (see above)
        U32 batches = 0;            // barrier batches recorded at a pass's start
        U32 inputs = 0;             // inputs named
        U32 inputs_moved = 0;       // moved to shader reading at the start
        U32 inputs_ready = 0;       // already readable
        U32 inputs_attached = 0;    // an attachment of the bound target (left to the draws)
        U32 inputs_unknown = 0;     // no such texture, or no image yet
        U32 targets_checked = 0;    // LLRHI_NATIVE_TARGETS: passes whose named target was compared with the bound one
        U32 targets_differ = 0;     //   and found different (then the bound one's attachments are not moved)
        U32 target_colors_moved = 0;
        U32 draws = 0;              // direct draws inside passes
        U32 without_draws = 0;      // passes that drew nothing
        // What the pass's last draw sampled (when it went the full path),
        // against the names: the declarations' check.
        U32 checked = 0;            // passes checked
        U32 undeclared = 0;         // sampled, not named (moved at the draw: the plan missed it)
        U32 unused = 0;             // named, not sampled (passes of one draw only)
        // Per pass name (a string literal), the first NAMES seen.
        const char* names[NAMES] = {};
        U32 name_passes[NAMES] = {};
        U32 name_draws[NAMES] = {};
        U32 name_moved[NAMES] = {};
        U32 name_undeclared[NAMES] = {};
        U32 name_unused[NAMES] = {};
        U32 other_names = 0;        // passes whose name found no entry left
        U64 ticks = 0;              // sPassBegin and sPassEnd, __rdtsc
    };
    Counts sCounts;

    struct OpenPass
    {
        bool open = false;
        U32 slot = NAMES;           // its name's entry in Counts (NAMES: none left)
        U32 draws_at_start = 0;     // sDirectDraws when it opened
        U32 hits_at_start = 0;      // sPassHits when it opened
        U32 frame = 0;
        Texture* inputs[LLRHI::PassDesc::MAX_INPUTS] = {};
        U32 input_count = 0;
    };
    OpenPass sOpen;
    bool sInstalled = false;

    U32 name_slot(const char* name)
    {
        Counts& c = sCounts;
        for (U32 i = 0; i < NAMES; ++i)
        {
            if (!c.names[i])
            {
                c.names[i] = name;
            }
            if (c.names[i] == name)
            {
                return i;
            }
        }
        return NAMES;
    }

    // The named target against the bound one: the same colour attachments in
    // the same order and the same size.
    bool same_target(const LLRHI::PassTarget& named, const ResolvedTarget& bound)
    {
        if (bound.is_default || named.color_count > 4 || bound.color_textures != named.color_count)
        {
            return false;
        }
        for (U32 i = 0; i < named.color_count; ++i)
        {
            if (!named.colors[i] || sTextures.find(named.colors[i]) != bound.colors[i])
            {
                return false;
            }
        }
        return bound.target.width == named.width && bound.target.height == named.height;
    }

    void close_pass()
    {
        if (!sOpen.open)
        {
            return;
        }
        Counts& c = sCounts;
        // sDirectDraws starts again at every perf report (LLVKGL::perfReport,
        // between frames): a pass is never open across frames, but if one
        // was, its count is lost.
        const U32 draws = sOpen.frame == sFrame && sDirectDraws >= sOpen.draws_at_start ? sDirectDraws - sOpen.draws_at_start : 0;
        c.draws += draws;
        c.without_draws += draws ? 0 : 1;
        if (sOpen.slot < NAMES)
        {
            c.name_draws[sOpen.slot] += draws;
        }
        // The last draw's sampled textures (tSampledUsed: through descriptors,
        // tBindlessSampled: through the bindless table), when it went the full
        // path: a pass cache hit leaves them as an earlier draw's. Compared as
        // pointers only.
        const bool full_path = sOpen.frame == sFrame && sPassHits == sOpen.hits_at_start;
        if (draws && full_path && tSampledUsed)
        {
            ++c.checked;
            auto sampled = [](const Texture* tex)
            {
                return std::find(tSampledUsed->begin(), tSampledUsed->end(), tex) != tSampledUsed->end() ||
                       std::find(tBindlessSampled.begin(), tBindlessSampled.end(), tex) != tBindlessSampled.end();
            };
            auto named = [](const Texture* tex)
            {
                return std::find(sOpen.inputs, sOpen.inputs + sOpen.input_count, tex) != sOpen.inputs + sOpen.input_count;
            };
            U32 undeclared = 0, unused = 0;
            for (const Texture* tex : *tSampledUsed)
            {
                undeclared += tex && !named(tex) ? 1 : 0;
            }
            for (const Texture* tex : tBindlessSampled)
            {
                undeclared += tex && !named(tex) ? 1 : 0;
            }
            if (draws == 1)
            {
                for (U32 i = 0; i < sOpen.input_count; ++i)
                {
                    unused += sOpen.inputs[i] && !sampled(sOpen.inputs[i]) ? 1 : 0;
                }
            }
            c.undeclared += undeclared;
            c.unused += unused;
            if (sOpen.slot < NAMES)
            {
                c.name_undeclared[sOpen.slot] += undeclared;
                c.name_unused[sOpen.slot] += unused;
            }
        }
        sOpen = OpenPass();
    }

    // LLRHI::sPassBegin.
    bool pass_begin(const LLRHI::PassDesc& pass)
    {
        if (!LLRHI::sNativePasses)
        {
            return false;
        }
        const U64 start = __rdtsc();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Counts& c = sCounts;
        if (sOpen.open)
        {
            ++c.replaced;
            close_pass();
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            return false;
        }
        ++c.passes;
        sOpen.open = true;
        sOpen.slot = name_slot(pass.name);
        sOpen.draws_at_start = sDirectDraws;
        sOpen.hits_at_start = sPassHits;
        sOpen.frame = sFrame;
        if (sOpen.slot < NAMES)
        {
            ++c.name_passes[sOpen.slot];
        }
        else
        {
            ++c.other_names;
        }

        // The bound framebuffer, as the pass's first draw will resolve it (the
        // resolution is cached: that draw finds it). Not while only counting
        // without the target's check: a pass that draws nothing would then
        // resolve a target the passes off never resolve.
        ResolvedTarget bound;
        const bool bound_ok = (LLRHI::sNativePasses == 1 || LLRHI::sNativeTargets) && resolve_draw_target(bound);
        bool target_named = false;
        if (LLRHI::sNativeTargets && pass.has_target)
        {
            ++c.targets_checked;
            target_named = bound_ok && same_target(pass.target, bound);
            c.targets_differ += target_named ? 0 : 1;
        }

        // The inputs, resolved once (also for the declarations' check).
        static std::vector<Texture*> moved;
        moved.clear();
        const U32 count = llmin(pass.input_count, LLRHI::PassDesc::MAX_INPUTS);
        for (U32 i = 0; i < count; ++i)
        {
            ++c.inputs;
            Texture* tex = pass.inputs[i] ? sTextures.find(pass.inputs[i]) : nullptr;
            if (!tex || !tex->image)
            {
                ++c.inputs_unknown;
                continue;
            }
            sOpen.inputs[sOpen.input_count++] = tex;
            if (bound_ok && (is_attachment_of(bound, tex) || tex == bound.depth))
            {
                ++c.inputs_attached;
                continue;
            }
            if (tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                ++c.inputs_ready;
                continue;
            }
            if (std::find(moved.begin(), moved.end(), tex) == moved.end())
            {
                moved.push_back(tex);
            }
        }

        if (LLRHI::sNativePasses != 1)
        {
            c.ticks += __rdtsc() - start;
            return true;    // 2: counted only
        }
        // No resolved target: the draws and clears skip too (a resize, a
        // minimized window), so the plan moves nothing either.
        if (!bound_ok || sCapture.on || depth_only_pass() || sActiveQuery || census_tag() || suppress_translation() || sOIT.active)
        {
            ++c.not_planned;
            c.ticks += __rdtsc() - start;
            return true;
        }
        U32 colors = 0;
        if (target_named)
        {
            for (U32 i = 0; i < bound.color_textures; ++i)
            {
                colors += bound.colors[i]->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? 1 : 0;
            }
        }
        if (!moved.empty() || colors)
        {
            VkCommandBuffer cmd = mirror.transferCommands();     // closes the rendering scope, as a draw's barriers do
            LLVKMirror::BarrierBatch batch(cmd);                // research/111 A-BB: one call
            for (Texture* tex : moved)
            {
                transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (colors)
            {
                for (U32 i = 0; i < bound.color_textures; ++i)
                {
                    transition(cmd, *bound.colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
            }
            ++c.batches;
            c.inputs_moved += (U32)moved.size();
            c.target_colors_moved += colors;
            if (sOpen.slot < NAMES)
            {
                c.name_moved[sOpen.slot] += (U32)moved.size() + colors;
            }
        }
        c.ticks += __rdtsc() - start;
        return true;
    }

    // LLRHI::sPassEnd.
    void pass_end()
    {
        const U64 start = __rdtsc();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        close_pass();
        sCounts.ticks += __rdtsc() - start;
    }
}

void LLVKPass::install()
{
    LLRHI::sPassBegin = &pass_begin;
    LLRHI::sPassEnd = &pass_end;
    sInstalled = true;
    LL_INFOS("VKPass") << "full-screen passes available: LLRHI_NATIVE_PASSES=" << LLRHI::sNativePasses << ", LLRHI_NATIVE_TARGETS="
                       << (LLRHI::sNativeTargets ? 1 : 0) << " (research/137 N12)" << LL_ENDL;
    // research/137 switch matrix 3: the targets are part of the passes
    // (pass_begin checks and, with =1, moves them), never on their own.
    if (LLRHI::sNativeTargets && !LLRHI::sNativePasses)
    {
        LL_WARNS("VKPass") << "LLRHI_NATIVE_TARGETS does nothing on its own: it needs LLRHI_NATIVE_PASSES=1 (or 2: the check "
                           << "counted only), now 0 (nativepasses.toggle turns the passes on)" << LL_ENDL;
    }
}

std::string LLVKPass::report(U32 frames)
{
    Counts c;
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        c = sCounts;
        sCounts = Counts();
    }
    if (!sInstalled || (!LLRHI::sNativePasses && !c.passes))
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    const F64 per = 1.0 / frames;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "LLRHI_NATIVE_PASSES=" << LLRHI::sNativePasses << " LLRHI_NATIVE_TARGETS=" << (LLRHI::sNativeTargets ? 1 : 0) << ": passes "
        << c.passes * per << " (replacing an open one " << c.replaced * per << ", without draws " << c.without_draws * per << ", draws "
        << c.draws * per << "), plan: batches " << c.batches * per << ", inputs named " << c.inputs * per << " moved "
        << c.inputs_moved * per << " ready " << c.inputs_ready * per << " attached " << c.inputs_attached * per << " unknown "
        << c.inputs_unknown * per << (LLRHI::sNativePasses == 1 || LLRHI::sNativeTargets ? "" : " (attached not told apart while counting only)")
        << ", target colours moved " << c.target_colors_moved * per << ", passes not planned "
        << c.not_planned * per << "; targets checked " << c.targets_checked * per << " differ " << c.targets_differ * per
        << "; declarations checked " << c.checked * per << ": sampled not named " << c.undeclared * per << ", named not sampled "
        << c.unused * per << "; by name (passes/draws/moved/not named/not sampled)";
    for (U32 i = 0; i < NAMES && c.names[i]; ++i)
    {
        if (c.name_passes[i])
        {
            out << " " << c.names[i] << " " << c.name_passes[i] * per << "/" << c.name_draws[i] * per << "/" << c.name_moved[i] * per
                << "/" << c.name_undeclared[i] * per << "/" << c.name_unused[i] * per;
        }
    }
    if (c.other_names)
    {
        out << " others " << c.other_names * per;
    }
    out << "; " << c.ticks / (perf_ticks_per_ns() * 1e6) * per << " ms";
    return out.str();
}

#else // !LL_GL_FUNC_POINTER

void LLVKPass::install()
{
}

std::string LLVKPass::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER
