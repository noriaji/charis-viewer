/**
 * @file llmissingobjects.h
 * @brief Diagnostics for objects in view that are not drawn.
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

#ifndef LL_LLMISSINGOBJECTS_H
#define LL_LLMISSINGOBJECTS_H

// Objects that are in the camera's view and within the draw distance but have
// nothing to draw (no drawable or group, orphaned, mesh not loaded, geometry
// never built, ...). Off unless LLRHI_MISSING_WATCH is set:
//   LLRHI_MISSING_WATCH=<seconds>  checks that often; objects that stay in a
//                                  suspicious state for 15 s are logged once,
//                                  with a count per state at every check
//   <LLRHI_CUE_DIR>/missing.dump   logs every object in view by state now
//   <LLRHI_CUE_DIR>/textures.dump  logs the textures of objects in view within
//                                  32 m (worn ones too) that are still short of
//                                  the resolution they want, with their fetch state
// Log tag: MissingObjects.
namespace LLMissingObjects
{
    // Once a frame on the main thread, with the world camera set up.
    void update();
}

#endif // LL_LLMISSINGOBJECTS_H
