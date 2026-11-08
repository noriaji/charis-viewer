/**
 * @file llrhipass.cpp
 * @brief Full-screen passes as units (research/137 N12): the switches and
 *        the hooks the backend sets.
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

#include "llrhipass.h"

#include "llrhi.h"      // LLRHI::envNumber

#include <cstdlib>

U32 LLRHI::sNativePasses = []()
{
    U64 v = 0;
    LLRHI::envNumber("LLRHI_NATIVE_PASSES", 0, ~(U64)0, v);
    return v == 1 || v == 2 ? (U32)v : 0u;
}();

bool LLRHI::sNativeTargets = []()
{
    U64 v = 0;
    LLRHI::envNumber("LLRHI_NATIVE_TARGETS", 0, ~(U64)0, v);
    return v > 0;
}();

bool (*LLRHI::sPassBegin)(const PassDesc&) = nullptr;
void (*LLRHI::sPassEnd)() = nullptr;
void (*LLRHI::sBindTarget)(U32, U32, S32, S32, S32, S32) = nullptr;
