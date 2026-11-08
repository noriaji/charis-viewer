/**
 * @file llrhiskinfill.cpp
 * @brief Skin fill: the switch and the backend's entry points.
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

#include "llrhiskinfill.h"

#include "llrhi.h"

bool LLRHI::sSkinFill = LLRHI::envDefaultOn("LLRHI_SKIN_FILL");

bool (*LLRHI::sSkinFillMask)(const SkinFillMask&) = nullptr;
bool (*LLRHI::sSkinFillShrink)(const SkinFillShrink&) = nullptr;
bool (*LLRHI::sSkinFillSoften)(const SkinFillSoften&) = nullptr;
bool (*LLRHI::sSkinFillOcclusion)(const SkinFillOcclusion&) = nullptr;
bool (*LLRHI::sSkinFillLight)(const SkinFillLight&) = nullptr;
