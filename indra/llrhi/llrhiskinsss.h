/**
 * @file llrhiskinsss.h
 * @brief Skin subsurface scattering: the skin mark and what the viewer hands
 *        the backend.
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

#ifndef LL_LLRHISKINSSS_H
#define LL_LLRHISKINSSS_H

#include "stdtypes.h"

// Research/100. While the viewer (plus/skinsss) marks skin, the main view's
// depth (LLPipeline's mRT->deferredScreen, shared by mRT->screen) carries a
// stencil: the G-buffer pass leaves STENCIL_SKIN set where the nearest opaque
// surface is skin (an avatar's Bakes on Mesh skin texture, or the system
// body), and STENCIL_EYE where it is that avatar's eyes. The passes below
// draw only there, through a stencil test, into the GL framebuffer bound when
// they are called (which must have that depth).
namespace LLRHI
{
    // Stencil bits of the main view's depth. The other bits are free for
    // other marks.
    constexpr U32 STENCIL_SKIN = 0x01;
    // The eyes of an avatar whose face is skin (the Bakes on Mesh eyes, the
    // system eyeballs, a rigged mesh weighted to the eyes only). Not skin
    // (no scattering), but lit as the face around them by passes that light
    // the face (plus/skinfill), so the eyes do not stay dark in a lit face.
    constexpr U32 STENCIL_EYE = 0x02;
    // Any avatar (its rigged meshes, the system body and eyeballs), at any
    // distance; not animesh. Written only while a feature asks for it
    // (plus::skinsss::addAvatarMarkUser: the wet look keeps avatars dry).
    constexpr U32 STENCIL_AVATAR = 0x04;
    // An avatar's bare skin and eyes, at any distance (what STENCIL_SKIN and
    // STENCIL_EYE mark near the camera, without the distance cut or the
    // face-is-skin rule: its Bakes on Mesh skin and eyes bakes, mesh eyes, the
    // system body and eyeballs). Written with STENCIL_AVATAR: the wet look's
    // snow does not lie on skin (the body's warmth melts it).
    constexpr U32 STENCIL_AVATAR_SKIN = 0x08;

    // Stage 1: the mark made visible (PlusSkinSssDebug). color, premultiplied
    // (rgb already times a), is laid over the bound target's RGB on the skin
    // pixels, eye_color likewise on the eye pixels; its alpha channel is kept.
    struct SkinSssDebug
    {
        F32 color[4] = {};
        F32 eye_color[4] = {};
    };

    // On unless LLRHI_SKIN_SSS=0 (a switch for tests: the depth then keeps
    // its official format, the stencil is never written and the passes are
    // never recorded). The viewer's settings decide whether they run.
    extern bool sSkinSss;

    // Set by the Vulkan backend. Records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded, e.g. the
    // framebuffer's depth has no stencil).
    extern bool (*sSkinSssDebug)(const SkinSssDebug& pass);
}

#endif // LL_LLRHISKINSSS_H
