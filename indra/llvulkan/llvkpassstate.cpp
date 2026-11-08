/**
 * @file llvkpassstate.cpp
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

#include "linden_common.h"

#include "llvkpassstate.h"

#include "llrhipassstate.h"
// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (this file has no PCH; as llvkpass.cpp).
#include "llvkgl_internal.h"
#include "llvktranslate.h"  // research/204 10 (vk-s4c2): the GL state's thread

#include <atomic>

#if LL_GL_FUNC_POINTER

// What a world pass's draw would use now, read from the tracked state the
// draw itself reads (sState: what set_dynamic_state and the pipeline key take;
// the bound program; the program's samplers' units and what is bound on
// them), for the viewer's comparison with the pass's declaration
// (LLRHI::comparePassState). Read only: nothing here changes state or records.
// Under sLock.
namespace
{
    using namespace llvkgl;

    class Backend final : public LLRHI::PassStateBackend
    {
    public:
        bool actual(LLRHI::PassStateActual& out) override
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            const State& s = sState;
            LLRHI::PassState& a = out.state;
            a.declared = 0;
            a.blend = s.blend;
            a.blend_src_rgb = s.blend_src_rgb;
            a.blend_dst_rgb = s.blend_dst_rgb;
            a.blend_src_a = s.blend_src_a;
            a.blend_dst_a = s.blend_dst_a;
            a.blend_eq_rgb = s.blend_eq_rgb;
            a.blend_eq_a = s.blend_eq_a;
            for (U32 i = 0; i < 4; ++i)
            {
                a.color_mask[i] = s.color_mask[i];
            }
            a.depth_test = s.depth_test;
            a.depth_write = s.depth_write;
            a.depth_func = s.depth_func;
            a.cull = s.cull;
            a.cull_face = s.cull_face;
            a.stencil = s.stencil;
            a.polygon_offset = s.polygon_offset;
            a.depth_clamp = s.depth_clamp;
            a.program = sCurrentProgram;
            a.sampler_units = 0;
            out.sampled_units = 0;
            out.bound_units = 0;
            const Program* program = sCurrentProgram ? current_program() : nullptr;
            out.program_known = program != nullptr;
            if (!program)
            {
                return true;
            }
            auto note = [&out](GLint unit, GLenum target)
            {
                if (unit < 0 || unit >= (GLint)LLRHI::PASS_STATE_UNITS)
                {
                    return;
                }
                const U32 bit = 1u << unit;
                out.sampled_units |= bit;
                if (sUnits[unit].names[target_slot(target)])
                {
                    out.bound_units |= bit;
                }
            };
            for (const SamplerSlot& slot : program->sampler_slots)
            {
                for (U32 e = 0; e < slot.count; ++e)
                {
                    const size_t element = (size_t)slot.first + e;
                    if (element < program->unit_values.size())
                    {
                        note(program->unit_values[element], slot.target);
                    }
                }
            }
            for (const Program::BindlessSampler& sampler : program->bindless)
            {
                note(sampler.unit, GL_TEXTURE_2D);  // the bindless rewrite takes 2D samplers only
            }
            return true;
        }

        bool counters(LLRHI::PassStateCounters& out) override
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            out.state_calls = sNativeStateCalls;
            U32 bindings = 0;
            for (U32 unit = 0; unit < 32; ++unit)
            {
                bindings += sUnitBindings[unit];
            }
            out.unit_bindings = bindings;
            out.program_changes = sProgramChanges;
            return true;
        }

        // research/204 10 (vk-s4c2): the units left, with each one's binding count now (a
        // later bind of it moves the count: then it is not "left" any more).
        bool unitsLeft(U32 units) override;
        U32 unitsRead() override;
    };

    Backend sBackend;

    // research/204 10 (vk-s4c2): LLRHI_PASS_UNITS' verify (the GL state's thread).
    thread_local U32 tLeftBinding[32] = {};
    std::atomic<U32> sLeftReads{ 0 };
    U32 sLeftLogged = 0;

    bool Backend::unitsLeft(U32 units)
    {
        if (LLVKTranslate::redirect())
        {
            return false;   // the GL state is the translation thread's (thread_local): not watched
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        for (U32 unit = 0; unit < 32; ++unit)
        {
            if (units & (1u << unit))
            {
                tLeftBinding[unit] = sUnitBindings[unit];
            }
        }
        LLVKPassState::tUnitsLeft |= units;
        return true;
    }

    U32 Backend::unitsRead()
    {
        return sLeftReads.load(std::memory_order_relaxed);
    }
}

namespace LLVKPassState
{
    using namespace llvkgl;

    thread_local U32 tUnitsLeft = 0;

    // A draw (or a capture of one, or a gathering of its descriptors) of the
    // program: each unit it reads that a cleanup left and nothing bound since
    // is counted once; a unit bound since is not left any more. Under sLock.
    void noteDraw(const llvkgl::Program& program)
    {
        auto note = [](GLint unit)
        {
            if (unit < 0 || unit >= 32 || !(tUnitsLeft & (1u << unit)))
            {
                return;
            }
            tUnitsLeft &= ~(1u << unit);
            if (sUnitBindings[unit] != tLeftBinding[unit])
            {
                return;     // bound again since the cleanup
            }
            sLeftReads.fetch_add(1, std::memory_order_relaxed);
            if (sLeftLogged < 8)
            {
                ++sLeftLogged;
                LL_WARNS("PassState") << "LLRHI_PASS_UNITS " << sLeftLogged << "/8: program " << sCurrentProgram << " reads unit " << unit
                                      << ", which a world pass's cleanup left unbound (on, it would read the pass's texture)" << LL_ENDL;
            }
        };
        for (GLint unit : program.unit_values)
        {
            note(unit);
        }
        for (const Program::BindlessSampler& sampler : program.bindless)
        {
            note(sampler.unit);
        }
    }

    void apply(const LLRHI::PassState& d, State& s)
    {
        using Group = LLRHI::PassState;
        if (d.declared & Group::BLEND)
        {
            s.blend = d.blend;
            if (d.blend)
            {
                s.blend_src_rgb = d.blend_src_rgb;
                s.blend_dst_rgb = d.blend_dst_rgb;
                s.blend_src_a = d.blend_src_a;
                s.blend_dst_a = d.blend_dst_a;
                s.blend_eq_rgb = d.blend_eq_rgb;
                s.blend_eq_a = d.blend_eq_a;
            }
        }
        if (d.declared & Group::COLOR_MASK)
        {
            for (U32 i = 0; i < 4; ++i)
            {
                s.color_mask[i] = d.color_mask[i];
            }
        }
        if (d.declared & Group::DEPTH)
        {
            s.depth_test = d.depth_test;
            if (d.depth_test)
            {
                s.depth_write = d.depth_write;
                s.depth_func = d.depth_func;
            }
        }
        if (d.declared & Group::CULL)
        {
            s.cull = d.cull;
            if (d.cull)
            {
                s.cull_face = d.cull_face;
            }
        }
        if (d.declared & Group::STENCIL)
        {
            s.stencil = d.stencil;
        }
        if (d.declared & Group::POLYGON_OFFSET)
        {
            s.polygon_offset = d.polygon_offset;
        }
        if (d.declared & Group::DEPTH_CLAMP)
        {
            s.depth_clamp = d.depth_clamp;
        }
    }
}

void LLVKPassState::install()
{
    LLRHI::sPassStateBackend = &sBackend;
    if (LLRHI::passStateMode() != LLRHI::PassStateMode::Off)
    {
        LL_INFOS("VKPassState") << "LLRHI_PASS_STATE=" << LLRHI::passStateModeName(LLRHI::passStateMode())
                                << ": the world passes' declarations (research/204)" << LL_ENDL;
    }
}

#else // !LL_GL_FUNC_POINTER

void LLVKPassState::install()
{
}

namespace LLVKPassState
{
    thread_local U32 tUnitsLeft = 0;
    void noteDraw(const llvkgl::Program&)
    {
    }
    void apply(const LLRHI::PassState&, llvkgl::State&)
    {
    }
}

#endif // LL_GL_FUNC_POINTER
