/**
 * @file llrhigtao.cpp
 * @brief GTAO and contact shadows: the switch and the backend's entry points.
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

#include "llrhigtao.h"

#include "llrhi.h"

bool LLRHI::sGtao = LLRHI::envDefaultOn("LLRHI_GTAO");

bool (*LLRHI::sGtaoDepth)(const GtaoDepth&) = nullptr;
bool (*LLRHI::sGtaoOcclusion)(const GtaoOcclusion&) = nullptr;
bool (*LLRHI::sGtaoDenoise)(const GtaoDenoise&) = nullptr;
bool (*LLRHI::sGtaoApply)(const GtaoApply&) = nullptr;
bool (*LLRHI::sContactShadows)(const ContactShadows&) = nullptr;
bool (*LLRHI::sGtaoApplyContact)(const GtaoApply&, const ContactShadows&) = nullptr;
