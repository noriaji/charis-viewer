/**
 * @file vkcheck.h
 * @brief Vulkan renderer start-up (plus/vkcheck): the GPU name kept across renderers, the GPU benchmark.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef PLUS_VKCHECK_H
#define PLUS_VKCHECK_H

#include <string>

namespace plus
{
namespace vkcheck
{
    // research/117 7.1: the same GPU named by GL's driver ("NVIDIA Corporation
    // NVIDIA GeForce RTX 2080/PCIe/SSE2") and by the Vulkan side's answers
    // (which may lack the bus and CPU suffixes, or differ in case and spaces).
    bool sameGPU(const std::string& a, const std::string& b);

    // LLFeatureManager::loadGPUClass, once the GPU string is known: a saved
    // LastGPUString that names the same GPU in the other form becomes this
    // one, so neither LLViewerWindow (applyRecommendedSettings) nor
    // LLStartUp ("DisplaySetToRecommendedGPUChange") takes it for a new GPU
    // and resets the graphics settings.
    void keepSameGPU(const std::string& gpu);

    // research/117 G6: LLFeatureManager::loadGPUClass's GPU benchmark. There
    // is no GL to time gpu_benchmark's draws with (it ran in a child process
    // on GL): the Vulkan backend makes the same draws in this process. True
    // when it did (gbps: GB/s as gpu_benchmark reports it, -1 when it could
    // not run, as gpu_benchmark's failures); false: no Vulkan backend.
    // research/117 9.6, 9.7: a raw result within 8 % of the same GPU's last
    // one keeps the higher of the two (PlusGPUBenchmarkGPU, PlusGPUBenchmarkGBps:
    // raw), so the class does not change from start to start around a class
    // line; then scaled once to what GL measures (x 1.05 on NVIDIA, 1 on
    // others until measured: PlusGPUBenchmarkVulkanScaleNVIDIA / Other).
    bool gpuBenchmark(float& gbps);
}
}

#endif // PLUS_VKCHECK_H
