/**
 * @file focuslock.h
 * @brief Focus lock: the depth of field focuses on a face (yours or another avatar's), on the point under the cursor,
 *        or on a place clicked on.
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

#ifndef PLUS_FOCUSLOCK_H
#define PLUS_FOCUSLOCK_H

#include <string>
#include <vector>

#include "indra_constants.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"

namespace plus::focuslock
{
    // Where the depth of field focuses (PlusFocusMode).
    enum Mode : S32
    {
        MODE_OFF    = 0,  // as the official viewer does (selected media, Alt zoom target, mouselook crosshair)
        MODE_CURSOR = 1,  // the point under the mouse cursor
        MODE_SELF   = 2,  // your own avatar
        MODE_OTHER  = 3,  // the avatar of PlusFocusAvatar
        MODE_CLICK  = 4,  // a point of the world clicked on (the picking below), held where it is in the world
        MODE_COUNT  = 5
    };

    // Which part of the avatar (PlusFocusPart).
    enum Part : S32
    {
        PART_EYES  = 0,  // between the two eyes
        PART_CHEST = 1,
        PART_BODY  = 2   // the pelvis
    };

    // What the lock does now, for the UI and the focus frame.
    enum class State
    {
        Off,       // mode 0
        BlurOff,   // a mode is chosen, but RenderDepthOfField is off: no focus is used at all
        Locked,    // the focus follows the target
        Lost       // the target is gone, not drawn, too small to trust, the cursor is over a panel, or
                   // no place is clicked yet (or it is behind the camera): the official focus is used meanwhile
    };

    // The mode, part and target; setters clamp and save the settings.
    // setMode() is the UI's choice: MODE_CLICK starts the picking (and forgets the point when it comes
    // from another mode), any other mode ends it.
    Mode getMode();
    void setMode(Mode mode);
    Part getPart();
    void setPart(Part part);
    LLUUID getAvatar();
    void setAvatar(const LLUUID& id);

    // The <plus> hook in LLPipeline::renderDoF(), where the focus point is decided and
    // before the official sources of it: true (and the agent-coordinates point set) when
    // the lock decides the focus. False leaves things as the official viewer does them.
    // Main thread, once a frame; returns at once while the mode is off. In mode 1 it
    // casts one ray (only called while the depth of field is on, as the mouselook focus is),
    // and a few more only when it meets see-through faces (throughGlass()).
    // Modes 2 and 3 use the joint's place itself: nothing between the camera and the
    // face (glass, alpha, other objects) changes the focus distance. Mode 4 uses the place
    // clicked (no ray a frame: the ray is cast once, at the click).
    bool focusPoint(LLVector3& agent_point);

    // Whether the lock decides the focus, and the reason when not. Cheap: no ray.
    State state();

    // The focus frame: where the target is on the screen (UI coordinates of the window,
    // origin at the bottom left, as LLCoordGL). False when the lock is not locked, or the
    // point is behind the camera or outside the world view. Mode 1 gives the cursor.
    bool focusScreenPos(LLVector2& pixels);

    // The distance from the camera to the focus, in meters. False when not locked.
    // Not for mode 1 (the ray hit is only known inside the pipeline frame): false.
    bool focusDistance(F32& meters);

    // Mode 4, "the place clicked" (research/264). While picking, a left click on the world reaches no
    // object (no touch, select, sit, drag, walk or teleport by double click): it puts the focus point
    // where it hit (the sky: far along the ray), and the next click moves it again. The point is kept in
    // global coordinates, so it stays on the same place when the camera moves; it is not saved (a session that
    // would start in mode 4 starts in mode 0, automatic: there is no point then), and a teleport forgets it. A click on a HUD (its clear click areas too) is the HUD's.
    // The picking ends with another mode, Esc, leaving the photo screen (photoscreen::leave()); the
    // point and the mode stay then. Nothing of the official code is changed while picking: the three
    // hooks below look at the flag on each event, so there is nothing to restore on any way out.
    bool isPicking();               // picking, and the mode is MODE_CLICK
    void setPicking(bool on);       // start (only in MODE_CLICK) or end the picking; shows or hides the hint (focushint.h)
    bool hasClickPoint();           // a place is clicked (it may be behind the camera now)

    // The <plus> hooks in LLViewerWindow (llviewerwindow.cpp), main thread:
    // handleAnyMouseClick(), where a click the UI did not take goes to the tool: true when it is the
    // picking's (a left press on the world, a double click's second press, and the release of a press
    // taken). Not with Alt (the Alt zoom), not on a HUD, not in mouselook. x, y: the scaled window
    // coordinates of the click (as getCurrentMouseX()).
    bool worldClick(EMouseClickType clicktype, bool down, MASK mask, S32 x, S32 y);
    // the frame's hover, after the tool's: the cross cursor over the world while picking. A few bool reads.
    void worldHover(MASK mask);
    // handleKey(), before the tool's: true (the key used) for Esc while picking, which ends it.
    bool worldKey(KEY key, MASK mask);

    // The target's name for the frame and the status line (the name format of the radar;
    // empty in mode 1 or when it is not known yet). Builds a string: not for every frame.
    std::string focusName();

    // The part's key ("eyes", "chest", "body"), as the UI's strings name it.
    const char* partKey(Part part);

    struct Nearby
    {
        LLUUID      id;
        std::string name;      // as the radar shows it; the account name's fallback while it is not loaded
        F32         distance;  // meters from the camera
    };

    // The avatars around the camera, nearest first, yourself not among them (nor
    // animesh control avatars). Allocates: for the UI's slow tick, not a frame.
    // Fills `out` (cleared first) and returns the count.
    size_t listNearby(std::vector<Nearby>& out, F32 radius = 128.f);

    // Focus through glass (PlusFocusThroughGlass, on by default): a see-through surface
    // of the world - less opaque than SEE_THROUGH_OPACITY, a window pane, a glass wall -
    // takes no focus, as a camera focuses through a window. Two places use it: the
    // cursor's ray (mode 1) goes on through such a face to what is behind it, and the
    // alpha pool's depth pass for depth of field (plus/dofalpha) writes no depth for
    // such pixels, so whatever is seen through the glass is blurred by its own distance
    // (a face behind a pane stays sharp when the focus is on it). Avatars and their
    // attachments are not "the world": hair and worn things keep their depth.
    constexpr F32 SEE_THROUGH_OPACITY = 0.9f;
    bool throughGlass();
}

#endif // PLUS_FOCUSLOCK_H
