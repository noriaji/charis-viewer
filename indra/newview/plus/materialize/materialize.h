/**
 * @file materialize.h
 * @brief The materialize band (research/256-materialize.md in the sl-viewer
 *        repository): its settings, colours and the count of the bands under
 *        way. The loading wireframe (llgraywire.h) takes a Tron look, and a
 *        surface shown as a wireframe until its texture came is swept from the
 *        bottom up by a band of light: wireframe above it, the real surface
 *        below. GrayWire draws it; the G-buffer shaders clip by its uniforms
 *        (materialize_plane / materialize_params / materialize_color, globalF.glsl).
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

#ifndef PLUS_MATERIALIZE_MATERIALIZE_H
#define PLUS_MATERIALIZE_MATERIALIZE_H

#include "stdtypes.h"
#include "v3color.h"

#include "plus/objectfx/objectfx_palette.h"

namespace plus::materialize
{
    // PlusMaterialize: the Tron look of the loading wireframe and the band
    // (both need RenderGrayTextureWireframe). Off: the plain cyan wireframe.
    bool on();

    // PlusMaterializeSeconds, clamped to 0.4 - 4: the band's way from the
    // bottom of a surface to its top.
    F32 seconds();

    // PlusMaterializePalette: 0 cyan, 1 orange, 2 magenta, 3 green (the table
    // plus/objectfx/objectfx_palette.h, shared with derez), PALETTE_CUSTOM:
    // PlusMaterializeColor.
    constexpr S32 PALETTE_CUSTOM = plus::objectfx::PALETTE_CUSTOM;

    // The colour of the lines and the band (sRGB, 0 - 1).
    LLColor3 colour();

    // Changes whenever on() or colour() does (GrayWire makes the look's
    // textures again then).
    U32 lookKey();

    // At most this many textures' bands at once (each holds its texture's
    // batches on the GL path, research/256 section 4): one past it comes
    // without a band.
    constexpr U32 MAX_BANDS = 512;

    // Whether a band from now until `until` may start; counted if so.
    bool admit(F32 now, F32 until);

    // research/259: PlusMaterializeAvatar (and on()): an avatar shown as a
    // wireframe as a whole is swept by one band from its feet to its head
    // when it has all it was waiting for.
    bool avatarOn();

    // At most this many avatars' bands at once (each draws its avatar's body
    // and attachments twice and holds them on the GL path): one past it
    // comes without a band.
    constexpr U32 MAX_AVATAR_BANDS = 8;

    // As admit, for an avatar's band.
    bool admitAvatar(F32 now, F32 until);

    // Whether a band may be under way: on(), and now before the end of the
    // last one admitted (a texture's or an avatar's). While false the batches
    // are not looked at.
    bool anyBand(F32 now);

    // research/278: the bands' clock (LLImageGL::sBandTime): the frame time,
    // but at most a step a frame, so that a hitch right after a texture came
    // does not use its band up unseen: twice the recent frame interval (an
    // average of the frames, each taken at most FRAME_TYPICAL_MAX: a longer one
    // is a hitch, not the frame rate), and at least BAND_STEP_MIN (review L1: at
    // a steady low frame rate the clock keeps to the frame time; at most 0.5 s
    // a frame, so it is slower under 2 fps, and for about a second after the
    // frame rate falls at once, until the average follows). The bands' starts, progress, ends and holds (mGrayWireDone,
    // admit / anyBand, mBandHold, an avatar's band) go by it, and (review M1)
    // when a waiting texture was last asked and drawn as a wireframe
    // (mGrayWireSeen, mGrayWireShown: a wait goes stale a second after on it);
    // the waits' own times (the grace, the give ups) by the frame time
    // (LLViewerTexture::sCurrentTime).
    constexpr F32 BAND_STEP_MIN = 0.05f;
    constexpr F32 FRAME_TYPICAL_MAX = 0.25f;
    F32 bandClock();

    // Once a frame (GrayWire::updateClass; a second call in a frame moves
    // nothing): the clock on to frame_time (gFrameTimeSeconds),
    // PlusMaterializeLog read, and its line every LOG_SECONDS.
    void tick(F32 frame_time);

    // research/278: PlusMaterializeLog (a debug setting), what the bands did,
    // to tell their causes apart on a live viewer: counted while it is on and
    // every LOG_SECONDS one line in the log (tag Materialize), the counts back
    // to 0. CAME_*: for each call of GrayWire::onImageCreated, why the texture
    // took no band, or that it took one (research/289: one drawn only where no
    // wireframe is, the shadow, reflection and probe passes, has no wait:
    // unwaited, no more not_shown; one with no GL name yet waits on and
    // may be counted again when its image comes; research/284: one whose band
    // waited for its alpha analysis counts MASK_WAIT then, and CAME_* when its
    // band starts, nothing there when it never does); AVATAR_*: as much for an avatar that
    // stopped being a wireframe (research/281: KEPT, a look at a materialized one
    // that would have made it a wireframe again, and did not); DRAWN_*: the main
    // view's draws of batches with a band under way (a sweep, a flash; research/282:
    // a flat batch's sweep along its longer horizontal side, a sweep by the bounds
    // of its last full rebuild when they were lost, a sweeping batch that flashes
    // for bounds that make no sense or under 0.1 m every way; a worn one, research/281: a
    // worn one in its own textures' flash, an avatar's body, a program
    // that does not take it, a pass that takes none, an alpha blended one),
    // the last two by render pass too, once a frame for a batch (pass 0: not
    // known, a batch registerFace did not make: grass, particles and the
    // like). The other DRAWN_* count each draw of a band. GRAY_* (research/289,
    // the integration check's M1): the main view's draws of batches not drawn
    // as a wireframe with a texture drawn as the gray stand-in for good: its
    // wait given up (GrayWire::isGray, mGrayWireGivenUp) while it is fetched
    // (after 300 s) or, pre-merge review L4, while nothing fetches it (after
    // 120 s: as when the VRAM's discard bias or its priority asks for none),
    // its asset missing; with or without a band, by render pass too, once a
    // frame for a batch. GRAY_IMAGE_FAILED, GRAY_TARGET_FAILED (L10): not draws
    // but the backend's failed tries since the line before to make the image
    // of a texture given its pixels (VRAM short and the like: it keeps its GL
    // name, drawn white, no wireframe; tried again at its next resolution, each
    // try counted), and of the other images (render targets, layered and depth
    // ones, the backend's 1 x 1 stand-ins: LLRHI::sTargetImageFailures). Only failures of
    // ensure_image when it creates an image are counted; rebuilds that keep the original image
    // (the attachable form, BC7 to plain, the move of compaction) are not.
    constexpr F32 LOG_SECONDS = 5.f;
    enum Tally : U8
    {
        CAME_UNWAITED, CAME_NO_IMAGE, CAME_OFF, CAME_WIREFRAME, CAME_NOT_SHOWN, CAME_GIVEN_UP, CAME_QUICK,
        CAME_CURTAIN, CAME_FULL, CAME_BAND,
        // research/284 (PlusAlphaMaskRebuild, GrayWire's alpha analyses): a band that waited for its texture's
        // analysis, one that waited too long; an analysis that came out as the faces were built, one that did
        // not, the groups built again for it (research/289: of them, those built under a second before);
        // a texture the list had no room for, one let go of after 30 s.
        MASK_WAIT, MASK_LATE, MASK_SAME, MASK_CHANGED, MASK_GROUPS, MASK_AGAIN, MASK_FULL, MASK_GAVE_UP,
        AVATAR_NOT_ALLOWED, AVATAR_CURTAIN, AVATAR_NOT_SEEN, AVATAR_NO_SKELETON, AVATAR_FULL, AVATAR_BAND, AVATAR_KEPT,
        DRAWN_SWEEP, DRAWN_FLASH, DRAWN_FLAT, DRAWN_LOST, DRAWN_ODD, DRAWN_WORN, DRAWN_WORN_OWN, DRAWN_BODY, DRAWN_NO_PROGRAM, DRAWN_NONE,
        DRAWN_ALPHA,
        GRAY_GIVEN_UP, GRAY_GIVEN_UP_IDLE, GRAY_MISSING,    // research/289
        GRAY_IMAGE_FAILED, GRAY_TARGET_FAILED,              // research/289 (L10): LLRHI::sTextureImageFailures, sTargetImageFailures
        TALLY_COUNT
    };
    // PlusMaterializeLog as of this frame's tick: the one test where nothing is counted.
    extern bool sLogOn;
    void addTally(Tally what, U32 pass);
    inline void tally(Tally what) { if (sLogOn) addTally(what, U32_MAX); }
    inline void tallyPass(Tally what, U32 pass) { if (sLogOn) addTally(what, pass); }

    // The band's edge for the shaders this frame (worked out once a frame):
    // its colour, linear, and the strength it is added with, capped for the
    // sky as the features that add light are (plus::effectiveExposure();
    // under plus::isNoPost() and a classic sky at most 0.9, under 1 after the
    // classic sky's final 1.1; its 1.35 is the sunlight's, which the unlit
    // edge does not take).
    struct Edge
    {
        F32 colour[4] = { 0.f, 0.f, 0.f, 1.f };
        F32 strength = 0.f;
    };
    const Edge& edge();
}

#endif // PLUS_MATERIALIZE_MATERIALIZE_H
