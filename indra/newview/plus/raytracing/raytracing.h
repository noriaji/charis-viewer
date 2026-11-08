/**
 * @file raytracing.h
 * @brief Hardware ray tracing for photos: an opt-in, never switched on by itself
 *        (research/230).
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

#ifndef PLUS_RAYTRACING_H
#define PLUS_RAYTRACING_H

#include <string>

#include "stdtypes.h"

// PlusRayTracing is 0 by default, is in no preset and is not touched by the
// quality presets, the automatic quality choice or the photo mode: only the
// person turns it on. Off costs nothing (nothing is built or kept). Main thread.
namespace plus::raytracing
{
    // The GPU and driver can do ray queries (the device enabled the extensions).
    bool available();

    // "on", or why the GPU cannot, for the settings panel and the log.
    std::string status();

    // The person turned it on and the GPU can. The later stages ask this
    // once per frame (LLCachedControl inside), and build nothing when false.
    bool enabled();

    // PlusRayTracingRange: metres around the camera whose static opaque geometry
    // goes into the acceleration structure (8 to 512).
    F32 range();
}

#endif // PLUS_RAYTRACING_H
