/**
 * @file llpassscope.cpp
 * @brief Full-screen passes as units (research/137 N12): the viewer's side.
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

#include "llpassscope.h"

#include "llfile.h"
#include "llframetimer.h"
#include "llrendertarget.h"
#include "llrhipass.h"
#include "llstring.h"

#include <string>

namespace
{
    // At most once a second (frame time): the cue files. The switch is read
    // per pass, so a toggle takes effect from the next pass on; a pass open
    // across it still closes (Scope keeps whether it opened).
    void poll()
    {
        static F64 next_poll = 0.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next_poll)
        {
            return;
        }
        next_poll = now + 1.0;

        static const std::string cue_dir = []()
        {
            return LLStringUtil::getenv("LLRHI_CUE_DIR");     // UTF-8, as LLFile takes it
        }();
        if (cue_dir.empty())
        {
            return;
        }
        // Without the backend's hooks neither cue changes anything: say so, as N8 does.
        static const char* const not_installed = "not installed, unchanged";
        // nativepasses.toggle: off, and back to the mode it had (1 when it started off).
        static U32 mode_on = LLRHI::sNativePasses ? LLRHI::sNativePasses : 1;
        const std::string toggle = cue_dir + "/nativepasses.toggle";
        if (LLFile::isfile(toggle) && LLFile::remove(toggle) == 0)
        {
            if (!LLRHI::sPassBegin)
            {
                LL_INFOS("RHIProfile") << "full-screen passes (research/137 N12): " << not_installed << LL_ENDL;
            }
            else
            {
                if (LLRHI::sNativePasses)
                {
                    mode_on = LLRHI::sNativePasses;
                    LLRHI::sNativePasses = 0;
                }
                else
                {
                    LLRHI::sNativePasses = mode_on;
                }
                LL_INFOS("RHIProfile") << "full-screen passes (research/137 N12) " << (LLRHI::sNativePasses ? "on" : "off")
                                       << " (LLRHI_NATIVE_PASSES=" << LLRHI::sNativePasses << ")" << LL_ENDL;
            }
        }
        // nativepassesref.toggle: the barrier plan (1) and counting only (2), while on.
        const std::string reference = cue_dir + "/nativepassesref.toggle";
        if (LLFile::isfile(reference) && LLFile::remove(reference) == 0)
        {
            if (!LLRHI::sPassBegin)
            {
                LL_INFOS("RHIProfile") << "full-screen passes' reference (research/137 N12): " << not_installed << LL_ENDL;
            }
            else if (!LLRHI::sNativePasses)
            {
                LL_INFOS("RHIProfile") << "full-screen passes' reference (research/137 N12): passes are off, unchanged" << LL_ENDL;
            }
            else
            {
                LLRHI::sNativePasses = LLRHI::sNativePasses == 1 ? 2 : 1;
                mode_on = LLRHI::sNativePasses;
                LL_INFOS("RHIProfile") << "full-screen passes' reference (research/137 N12) " << (LLRHI::sNativePasses == 2 ? "on" : "off")
                                       << " (LLRHI_NATIVE_PASSES=" << LLRHI::sNativePasses << ")" << LL_ENDL;
            }
        }
    }
}

LLPassScope::Input::Input(const LLRenderTarget* target)
{
    if (target && target->getNumTextures() > 0)
    {
        mName = target->getTexture(0);
    }
}

// static
LLPassScope::Input LLPassScope::attachment(const LLRenderTarget* target, U32 index)
{
    return Input(target && index < target->getNumTextures() ? target->getTexture(index) : 0u);
}

// static
LLPassScope::Input LLPassScope::depth(const LLRenderTarget* target)
{
    return Input(target ? target->getDepth() : 0u);
}

// static
LLPassScope::Input LLPassScope::texture(U32 name)
{
    return Input(name);
}

LLPassScope::LLPassScope(const char* name, const LLRenderTarget* target, std::initializer_list<Input> inputs)
{
    poll();
    if (!LLRHI::sNativePasses || !LLRHI::sPassBegin)
    {
        return;
    }
    LLRHI::PassDesc pass;
    pass.name = name ? name : "";
    if (target)
    {
        LLRHI::PassTarget& t = pass.target;
        pass.has_target = true;
        // Opened right after bindTarget: the bound framebuffer is the target's.
        t.framebuffer = LLRenderTarget::getCurrentBoundTarget() == target ? LLRenderTarget::sCurFBO : 0;
        t.color_count = llmin(target->getNumTextures(), 4u);
        for (U32 i = 0; i < t.color_count; ++i)
        {
            t.colors[i] = target->getTexture(i);
        }
        t.depth = target->getDepth();
        t.width = target->getWidth();
        t.height = target->getHeight();
    }
    for (const Input& input : inputs)
    {
        if (input.name() && pass.input_count < LLRHI::PassDesc::MAX_INPUTS)
        {
            pass.inputs[pass.input_count++] = input.name();
        }
    }
    mOpen = LLRHI::sPassBegin(pass);
}

void LLPassScope::close() const
{
    if (mOpen && LLRHI::sPassEnd)
    {
        LLRHI::sPassEnd();
    }
    mOpen = false;
}
