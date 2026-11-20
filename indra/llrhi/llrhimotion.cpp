/**
 * @file llrhimotion.cpp
 * @brief The motion vectors of the main view's G-buffer (research/373): the
 *        switches and the backend's entry points.
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

#include "llrhimotion.h"

#include "llrhi.h"

namespace
{
    U32 motionViewMode()
    {
        U64 mode = LLRHI::MOTION_VIEW_OFF;
        LLRHI::envNumber("LLRHI_MOTION_VIEW", LLRHI::MOTION_VIEW_OFF, LLRHI::MOTION_VIEW_COUNT - 1, mode);
        return (U32)mode;
    }
}

bool LLRHI::sMotion = LLRHI::envDefaultOn("LLRHI_TEMPORAL");
U32 LLRHI::sMotionViewMode = motionViewMode();

bool (*LLRHI::sMotionClear)(U32, U32) = nullptr;
bool (*LLRHI::sMotionCamera)(const MotionCameraPass&) = nullptr;
bool (*LLRHI::sMotionViewDraw)(const MotionViewPass&) = nullptr;
