/**
 * @file lllocallights.cpp
 * @brief The local light table (research/197 stage 3c, research/203).
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

#include "llviewerprecompiledheaders.h"

#include "lllocallights.h"

#include "llappviewer.h"    // gFrameTimeSeconds
#include "lldrawable.h"
#include "llprimitive.h"    // LLNetworkData
#include "llrhi.h"

LLLocalLightTable gLocalLightTable;

namespace
{
    constexpr U32 LOGGED_MISMATCHES = 8;
    constexpr F32 REPORT_SECONDS = 5.f;
}

bool LLLocalLightTable::reads()
{
    return LLRHI::sLightTable && !LLRHI::sLightTableVerify;
}

bool LLLocalLightTable::verifies()
{
    return LLRHI::sLightTableVerify;
}

void LLLocalLightTable::paramsChanged(LLDrawable* drawable, U16 param_type)
{
    if (param_type == LLNetworkData::PARAMS_LIGHT || param_type == LLNetworkData::PARAMS_LIGHT_IMAGE)
    {
        mSlots.markDirty(drawable);
    }
}

void LLLocalLightTable::reportMismatch(const char* field, const LLLocalLights::Values& was, const LLLocalLights::Values& now)
{
    if (mLoggedMismatches >= LOGGED_MISMATCHES)
    {
        return;
    }
    ++mLoggedMismatches;
    LL_WARNS("LightTable") << "light table: a kept record differs from its light in " << field << " (kept center " << was.center[0] << ", "
                           << was.center[1] << ", " << was.center[2] << ", radius " << was.radius << ", flags " << was.flags
                           << "; now center " << now.center[0] << ", " << now.center[1] << ", " << now.center[2] << ", radius "
                           << now.radius << ", flags " << now.flags << ")" << LL_ENDL;
}

void LLLocalLightTable::beginFrame()
{
    // auto: the frame before's comparisons.
    if (LLRHI::sLightTableAuto && LLRHI::sLightTableVerify)
    {
        if (mFrame.mismatches)
        {
            LLRHI::sLightTableAuto = false;
            LLRHI::sLightTableVerify = false;
            LLRHI::sLightTable = false;
            LL_WARNS("LightTable") << "light table [auto]: " << mFrame.mismatches << " record(s) differed from their light after "
                                   << mAutoCleanFrames << " clean frames; off for this run" << LL_ENDL;
        }
        else if (mFrame.checked && ++mAutoCleanFrames >= AUTO_FRAMES)
        {
            LLRHI::sLightTableAuto = false;
            LLRHI::sLightTableVerify = false;
            LL_INFOS("LightTable") << "light table [auto]: " << mAutoCleanFrames
                                   << " frames compared with no difference; on (verify off)" << LL_ENDL;
        }
    }

    mPeriod.add(mFrame);
    mFrame = LLLocalLights::Counts();
    ++mPeriodFrames;

    if (mLoggedAt < 0.f)
    {
        mLoggedAt = gFrameTimeSeconds;
    }
    if (gFrameTimeSeconds - mLoggedAt < REPORT_SECONDS)
    {
        return;
    }
    if (LLRHI::sLightTableVerify || LLRHI::sFrameCost)
    {
        const F32 per = 1.f / (F32)llmax(mPeriodFrames, 1u);
        LL_INFOS("LightTable") << llformat("light table [%s] over %u frames: records %u (slots %u); per frame from the table %.1f, "
                                           "read again %.1f, from the object %.1f; compared %u, differences %u (must stay 0)",
                                           LLRHI::sLightTableAuto ? "auto" : LLRHI::sLightTableVerify ? "verify" : reads() ? "on" : "off",
                                           mPeriodFrames, mSlots.live(), mSlots.span(), mPeriod.table * per, mPeriod.refreshed * per,
                                           mPeriod.object * per, mPeriod.checked, mPeriod.mismatches)
                               << LL_ENDL;
    }
    mLoggedAt = gFrameTimeSeconds;
    mPeriod = LLLocalLights::Counts();
    mPeriodFrames = 0;
}
