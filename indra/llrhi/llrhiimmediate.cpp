/**
 * @file llrhiimmediate.cpp
 * @brief The immediate mode's fast path (research/137 N9): the switch and the hook.
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

#include "llrhiimmediate.h"

// Off unless set and not starting with '0'.
bool LLRHI::sImmediateRing = !LLRHI::envUnset("LLRHI_IMMEDIATE_RING") && LLRHI::envFirstChar("LLRHI_IMMEDIATE_RING") != '0';
bool (*LLRHI::sImmediateDraw)(const ImmediateDraw&) = nullptr;
std::atomic<U32> LLRHI::sVBCacheMisses{ 0 };
