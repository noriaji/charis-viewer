/**
 * @file derez.h
 * @brief Derez: a deleted object crumbles into glowing blocks that fly up, instead of vanishing at once.
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

#ifndef PLUS_DEREZ_H
#define PLUS_DEREZ_H

#include "stdtypes.h"

class LLGLSLShader;
class LLViewerObject;

// research/257. When the simulator kills an object the viewer has drawn lately and near the camera
// (taken, returned, deleted: anyone's), the faces of its linkset are kept for PlusDerezSeconds as a
// "ghost": the vertex buffer ranges, textures, colours and model matrices as they were (values, no
// LLFace or LLDrawable). A program of its own (deferred/derezV.glsl, derezF.glsl, derezClock.glsl) draws the ghosts
// into the main view's G-buffer after the opaque pools: the object's box is cut into cells, the cells
// light up and crumble from the top down into blocks that rise, shrink and fade out, with the cuts and
// the blocks glowing (the emissive target). Lighting, haze, water, SSR and TAA take them as any other
// opaque surface. No shadow, no reflection probe, no water mirror, no impostor.
//
// Left out: avatars, attachments and HUDs, rigged meshes, trees, grass, physical or moving objects,
// an object someone sits on, objects at a region's edge, small temporary objects (bullets), the kills
// of a teleport or a region change (and 3 seconds after one), and faces that are invisible. Budgets
// (ghosts, faces, vertices, buffer bytes) keep the held VRAM and the cost bounded; past them the
// object vanishes as before. Main thread only (the kill comes in idle(), the draw in display()).
namespace plus::derez
{
    namespace detail
    {
        // Ghosts being drawn: 0 makes render() one comparison.
        extern U32 sLiveCount;
        void render();
    }

    // process_kill_object(), just before gObjectList.killObject(objectp): starts the object's ghost when
    // it qualifies (PlusDerez on, the checks above, a slot and the budget free). Takes nothing from the
    // object after it returns.
    void onKill(LLViewerObject* objectp);

    // LLPipeline::renderGeomDeferred(), after the opaque pools: draws the live ghosts into the main
    // view's G-buffer (nothing in a probe face, a mirror, a shadow map, an impostor or a HUD), and lets
    // go of the ones whose time is over.
    inline void render()
    {
        if (detail::sLiveCount != 0)
        {
            detail::render();
        }
    }

    // Lets go of every ghost (vertex buffers, textures) now: the agent's region changed, a teleport
    // began, PlusDerez went off, the app is quitting (before the renderer is shut down).
    void clear();
}

// The ghosts' G-buffer program (llviewershadermgr.cpp loads and unloads it with the deferred ones).
extern LLGLSLShader gDeferredDerezProgram;

#endif // PLUS_DEREZ_H
