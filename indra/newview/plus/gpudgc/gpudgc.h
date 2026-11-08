/**
 * @file gpudgc.h
 * @brief DGC by the setting PlusGpuDrivenDGC (plus/gpudgc): what the Vulkan
 *        exit (plus/vkcheck) asks of it.
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

#ifndef PLUS_GPUDGC_H
#define PLUS_GPUDGC_H

namespace plus
{
namespace gpudgc
{
    enum class Loss
    {
        NotCounted,     // DGC did not run this start, or the environment or "off" decides
        Counted,        // the first start in a row: DGC stays, and goes off if the next one ends alike
        TurnedOff,      // the second in a row: the setting is "off" from the next start
    };

    // fix-round9b 4, fix-round10 3: the device was lost and the viewer ends
    // (not a failed submit: most often out of memory, not DGC's doing). When
    // DGC ran this start (LLRHI::sGPUDrivenDGCUsed) by the setting (auto or
    // on, not the environment), the loss is counted (PlusGpuDrivenDGCLostCount);
    // the second start in a row so ended turns the setting "off" for the next
    // start, so that a GPU or driver that DGC brings down does not end every
    // start, while one loss for another reason (heat, a driver update) leaves
    // it. The caller saves the settings, and its message says which it was.
    Loss onDeviceLost();
}
}

#endif // PLUS_GPUDGC_H
