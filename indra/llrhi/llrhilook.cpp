/**
 * @file llrhilook.cpp
 * @brief Looks (colour grading with a LUT): the switch and the backend's entry point.
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

#include "llrhilook.h"

#include "llrhi.h"

#include <cstdlib>

bool LLRHI::sLook = LLRHI::envDefaultOn("LLRHI_LOOK");

U32 LLRHI::sLookDebug = []()
{
    U64 value = 0;
    LLRHI::envNumber("LLRHI_LOOK_DEBUG", 0, U32_MAX, value);
    return (U32)value;
}();

bool (*LLRHI::sLookGrade)(const LookGrade&) = nullptr;
