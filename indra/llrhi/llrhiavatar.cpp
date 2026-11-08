/**
 * @file llrhiavatar.cpp
 * @brief Stage 4b (research/199): the switches and the backend's entry point.
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

#include "llrhiavatar.h"
#include "llrhiscene.h"     // research/219: sceneRiggedMode

#include <cstdlib>
#include <cstring>

namespace
{
    LLRHIAvatar::Mode read_mode(const char* name, bool allow_auto)
    {
        const char* env = std::getenv(name);
        if (!env || !*env || !strcmp(env, "0") || !strcmp(env, "off"))
        {
            return LLRHIAvatar::MODE_OFF;
        }
        if (!strcmp(env, "verify"))
        {
            return LLRHIAvatar::MODE_VERIFY;
        }
        if (!strcmp(env, "auto"))
        {
            return allow_auto ? LLRHIAvatar::MODE_AUTO : LLRHIAvatar::MODE_VERIFY;
        }
        if (!strcmp(env, "1") || !strcmp(env, "on"))
        {
            return LLRHIAvatar::MODE_ON;
        }
        return LLRHIAvatar::MODE_OFF;   // not a value of the switch (read before logging starts: no warning)
    }

    // research/219: unset while LLRHI_SCENE_RIGGED is not off, the rows are
    // written as verify (the scene's rigged records read them; the usual
    // draws read their rolling rows as before). Set, as asked (off: the
    // rigged batches keep no records either).
    LLRHIAvatar::Mode palette_switch()
    {
        const char* env = std::getenv("LLRHI_AVATAR_PALETTE");
        if ((!env || !*env) && LLRHI::sceneRiggedMode() != LLRHI::SceneMode::Off)
        {
            return LLRHIAvatar::MODE_VERIFY;
        }
        return read_mode("LLRHI_AVATAR_PALETTE", true);
    }

    const LLRHIAvatar::Mode sPaletteSwitch = palette_switch();
    const LLRHIAvatar::Mode sSkinSwitch = read_mode("LLRHI_COMPUTE_SKIN", false);

    // What the palette rows do now: off, verify (auto's first half too) or on.
    // Main thread only (the builds and the backend's checks run there).
    LLRHIAvatar::Mode sPaletteNow = sPaletteSwitch == LLRHIAvatar::MODE_AUTO ? LLRHIAvatar::MODE_VERIFY : sPaletteSwitch;
    bool sPaletteAuto = sPaletteSwitch == LLRHIAvatar::MODE_AUTO;
    U32 sAutoCleanFrames = 0;

    LLRHIAvatar::Backend* sBackend = nullptr;
}

const char* LLRHIAvatar::modeName(Mode mode)
{
    switch (mode)
    {
    case MODE_VERIFY:
        return "verify";
    case MODE_AUTO:
        return "auto";
    case MODE_ON:
        return "on";
    default:
        return "off";
    }
}

LLRHIAvatar::Mode LLRHIAvatar::paletteMode()
{
    return sPaletteSwitch;
}

bool LLRHIAvatar::paletteWrites()
{
    return sBackend && sPaletteNow != MODE_OFF;
}

bool LLRHIAvatar::paletteDraws()
{
    return sBackend && sPaletteNow == MODE_ON;
}

void LLRHIAvatar::notePaletteChecks(U32 checked, U32 mismatches)
{
    if (!sPaletteAuto || sPaletteNow != MODE_VERIFY)
    {
        return;
    }
    if (mismatches)
    {
        // auto: the old way for the rest of the run (the rows stay allocated
        // and are given back as usual; nothing draws from them).
        sPaletteAuto = false;
        sPaletteNow = MODE_OFF;
        LL_WARNS("RHIAvatar") << "avatar palette rows [auto]: " << mismatches << " mismatch(es) after " << sAutoCleanFrames
                              << " clean frames; off for this run" << LL_ENDL;
        return;
    }
    if (checked && ++sAutoCleanFrames >= AUTO_FRAMES)
    {
        sPaletteAuto = false;
        sPaletteNow = MODE_ON;
        LL_INFOS("RHIAvatar") << "avatar palette rows [auto]: " << sAutoCleanFrames
                              << " frames checked with no mismatch; on (verify off)" << LL_ENDL;
    }
}

LLRHIAvatar::Mode LLRHIAvatar::computeSkinMode()
{
    return sSkinSwitch;
}

bool LLRHIAvatar::computeSkinVerify()
{
    return sBackend && sSkinSwitch != MODE_OFF;
}

void LLRHIAvatar::setBackend(Backend* backend)
{
    sBackend = backend;
    if (backend && (sPaletteSwitch != MODE_OFF || sSkinSwitch != MODE_OFF))
    {
        LL_INFOS("RHIAvatar") << "stage 4b (research/199): LLRHI_AVATAR_PALETTE " << modeName(sPaletteSwitch)
                              << ", LLRHI_COMPUTE_SKIN " << modeName(sSkinSwitch) << LL_ENDL;
        if (sSkinSwitch == MODE_ON)
        {
            LL_INFOS("RHIAvatar") << "LLRHI_COMPUTE_SKIN=on: drawing from compute skinned vertices waits for stage 1's "
                                     "vertex placement (research/199 2.2); runs as verify" << LL_ENDL;
        }
    }
}

LLRHIAvatar::Backend* LLRHIAvatar::backend()
{
    return sBackend;
}
