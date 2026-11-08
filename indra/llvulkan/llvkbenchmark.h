/**
 * @file llvkbenchmark.h
 * @brief The GPU benchmark the viewer's GPU class comes from, on Vulkan (research/117 9.3).
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

#ifndef LL_LLVKBENCHMARK_H
#define LL_LLVKBENCHMARK_H

#include "stdtypes.h"

#include <string>

// The official viewer's gpu_benchmark() (llglsandbox.cpp) times, with GL's
// timer and samples-passed queries, 64 rounds of a full-screen triangle
// sampling each of 32 textures of 1024x1024 RGBA8 into a 1024x1024 RGBA8
// target, and calls 4 bytes per sample over that time the GPU's memory
// bandwidth (GB/s); LLFeatureManager::loadGPUClass turns it into the GPU
// class. It ran in a child process on plain GL. Without GL the same draws are
// recorded here on Vulkan, timed with timestamps (every fragment is drawn:
// samples = draws x 1024 x 1024).
namespace LLVKBenchmark
{
    // GB/s as gpu_benchmark() reports it for one run, or -1 when it could not
    // run: the median of several runs once the GPU has warmed up (research/117
    // 9.5; gpu_benchmark takes the second of two). sensors (may be null): a
    // line of the GPU's clocks for the log. Main thread, the device ready.
    F32 memoryBandwidth(std::string (*sensors)());
}

#endif // LL_LLVKBENCHMARK_H
