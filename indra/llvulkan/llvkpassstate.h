/**
 * @file llvkpassstate.h
 * @brief The Vulkan backend's side of the world passes' declared states (research/204).
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

#ifndef LL_LLVKPASSSTATE_H
#define LL_LLVKPASSSTATE_H

#include "stdtypes.h"

namespace LLRHI
{
    struct PassState;
}
namespace llvkgl
{
    struct State;
    struct Program;
}

namespace LLVKPassState
{
    // Sets LLRHI::sPassStateBackend. Called from LLRHIVKMirror::install
    // after LLVKGL::install. Installed whatever
    // LLRHI_PASS_STATE says (it costs nothing while no scope asks).
    void install();

    // research/204 10 (vk-s4c2): the declared groups of a pass state over a
    // tracked state, as a draw would use them (the factors while blending, the
    // depth write and function while testing, the face while culling; the
    // stencil, polygon offset and depth clamp on or off). Pure.
    void apply(const LLRHI::PassState& declared, llvkgl::State& state);

    // research/204 10 (vk-s4c2): LLRHI_PASS_UNITS' verify in the backend.
    // The units a pass's cleanup left unbound (the thread whose GL state it
    // is): a draw whose program reads one before it is bound again is
    // counted once (noteDraw, at the draws' entries: draw_into_target, the
    // probe list's capture, gather_descriptors). 0 while nothing is left.
    extern thread_local U32 tUnitsLeft;
    void noteDraw(const llvkgl::Program& program);
}

#endif // LL_LLVKPASSSTATE_H
