/**
 * @file llrhipass.h
 * @brief Full-screen passes as units (research/137 N12): the post chain and
 *        the lighting passes of LLPipeline name their target and the textures
 *        they sample, and the backend plans the pass's barriers at its start.
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

#ifndef LL_LLRHIPASS_H
#define LL_LLRHIPASS_H

#include "stdtypes.h"

// Research/137 N12 (a: the post chain, visualizeBuffers to renderFinalize;
// b: lighting and atmospherics, renderDeferredLighting to doWaterHaze).
//
// A pass is the stretch of a full-screen step between the viewer binding its
// target and the step's end: usually one screen triangle, sometimes a few
// draws into the same target (local lights). The viewer opens it right after
// binding the target (LLPassScope, llrender) and says what it is: a name,
// the target as a value (PassTarget: the render target as an object the
// backend resolves by itself, the first step of research/137 N12c) and the
// textures the pass samples. Everything else goes as before: the same
// programs, uniforms, texture units and draws through the same hooks, so the
// draws and their state are the same with the passes on or off.
//
// What changes with LLRHI_NATIVE_PASSES=1 is when the layout changes happen:
// the backend moves the named textures to shader reading in one barrier
// batch when the pass begins, before its first draw opens the rendering
// scope (the pass's barrier plan), instead of each draw finding them itself.
// With LLRHI_NATIVE_TARGETS=1 the target's colour attachments join that batch
// (moved to attachment layout): the target as the viewer named it, checked
// against the framebuffer it bound. Nothing else is recorded: a texture the
// pass did not name still moves at its draw, as before; a named one already
// readable costs nothing.
//
// LLRHI_NATIVE_PASSES=2 (the reference for A/B images): the passes are opened
// and counted, the names checked against what the draws sampled, and nothing
// is recorded or moved (with LLRHI_NATIVE_TARGETS=0: the same commands as
// with the passes off; the target's check resolves the bound framebuffer,
// which the pass's first draw would do anyway).
namespace LLRHI
{
    // The render target of a pass, as LLRenderTarget holds it.
    struct PassTarget
    {
        U32 framebuffer = 0;        // the GL framebuffer the viewer bound for the pass (0: not known)
        U32 colors[4] = {};         // GL names of its colour attachments, in draw buffer order
        U32 color_count = 0;
        U32 depth = 0;              // GL name of its own depth texture (0: none, or another target's shared)
        U32 width = 0;
        U32 height = 0;
    };

    struct PassDesc
    {
        static constexpr U32 MAX_INPUTS = 12;
        const char* name = "";      // a string literal (the counts are kept by its address)
        bool has_target = false;    // false: the frame's own target, or one the viewer did not name
        PassTarget target;
        U32 inputs[MAX_INPUTS] = {};// GL names of the textures the pass samples
        U32 input_count = 0;
    };

    // LLRHI_NATIVE_PASSES: 0 off (default), 1 the barrier plan, 2 counted only.
    // Switched while running by the cue files nativepasses.toggle (off and
    // back) and nativepassesref.toggle (1 and 2), in llrender/llpassscope.cpp.
    extern U32 sNativePasses;
    // LLRHI_NATIVE_TARGETS (startup, off by default): the backend resolves the
    // pass's target from PassTarget, checks it against the bound framebuffer
    // and, with LLRHI_NATIVE_PASSES=1, moves its colour attachments too
    // (=2: the check is counted only). Nothing while LLRHI_NATIVE_PASSES is 0
    // (LLVKPass::install warns).
    extern bool sNativeTargets;

    // Set by the Vulkan backend. Render thread only.
    // sPassBegin opens the pass (a pass still open is closed first); false:
    // not opened, nothing to close. sPassEnd closes the open pass (none: nothing).
    extern bool (*sPassBegin)(const PassDesc& pass);
    extern void (*sPassEnd)();

    // research/156 N12c (LLRHI_BIND_TARGET=1 at startup, off by default):
    // LLRenderTarget binds its target in one call instead of four (the
    // framebuffer, its draw and read buffers, the viewport). framebuffer 0:
    // the frame's own target (its buffers are GL_BACK: nothing to name).
    // color_count: the draw buffers GL_COLOR_ATTACHMENT0.. (0: GL_NONE).
    // The backend does what the four calls did, in their order. Set by the
    // Vulkan backend; render thread.
    extern void (*sBindTarget)(U32 framebuffer, U32 color_count, S32 x, S32 y, S32 width, S32 height);
}

#endif // LL_LLRHIPASS_H
