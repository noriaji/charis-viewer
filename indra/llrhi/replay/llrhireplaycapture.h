/**
 * @file llrhireplaycapture.h
 * @brief research/271 P1b stage 2: records llrhi entry points to a .crp file (LLRHIReplay).
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

#ifndef LL_LLRHIREPLAYCAPTURE_H
#define LL_LLRHIREPLAYCAPTURE_H

// research/271 P1b stage 2, first entry (LLRHI::sDirectDraw: the busiest one, research/168 /
// research/270: 4000-5000 calls a frame). Only this one entry is wrapped so far; the rest of
// research/271 2's boundary (67 GL calls, 135 more hooks, Scene's 28 methods...) are not yet
// touched. The intent (research/271 9, P1b stage 2) is to prove the mechanism end to end on
// the highest-value entry first, then widen it, generating the per-entry wrap and the argument
// copy from indra/llrhi/replay/entry_manifest.json instead of writing it by hand as here.
//
// Off (LLRHI_REPLAY_CAPTURE unset): install() does nothing at all, LLRHI::sDirectDraw is left
// untouched, and nothing here runs (research/271 3.1's "スイッチがオフの時...費用はゼロ").
//
// On: LLRHI_REPLAY_CAPTURE=<path> names the .crp file; LLRHI_REPLAY_CAPTURE_FRAMES (default 3)
// is how many frames (present_frame calls) to record before closing the file and going back to
// calling the real backend function directly with no recording. LLRHI_REPLAY_CAPTURE_MAX_MB
// (default 1024) caps the file: the capture stops there (one log line) with every record before
// it intact. Both are whole numbers (a value that is not one: a warning, the default kept).
// Records are written out as they come (llrhireplayformat.h's Writer: two 4 MB buffers, one
// written by a thread of its own while the other fills), so memory stays bounded; running out
// of memory ends the capture (closed at the next frame's end), never the viewer. With direct
// draws off (LLRHI_DIRECT=0) there is nothing to record: a warning, and no capture.
//
// Not yet done (left for the next entries, research/271 3.2 / 3.3 / 3.6):
//  - no identity table: a GL name, a TexRef or LLRHI::DirectDraw::draw_template's value is
//    written as a plain number, not translated on replay.
//  - no resource dump (vertex/index buffer contents, textures, programs): a capture taken now
//    can be read back and inspected, not replayed through to a drawn frame.
//  - no manifest hash check (research/271 3.2-1): the file format's own version is checked
//    (LLRHIReplay::FORMAT_VERSION), the boundary's shape is not.
//  - one entry, so no per-thread recursion guard (research/271 3.6 point 1) is needed yet:
//    LLRHI::sDirectDraw is a hook called directly (lldrawpool.cpp's pushDirectBatch), not a GL
//    call with Builder::cut() folded into the same wrapper function (that is LLVKGL_REPLACE's
//    shape, not this one's). Calling install() after LLRHI::DrawList::installGuard()
//    (llrhivkmirror.cpp) wraps whichever function LLRHI::sDirectDraw holds at that moment: if
//    the draw list guard already wrapped it there (installGuard() ran guard_hooks(): draw
//    lists were on and ready), this wrapper becomes the outer of the two; if draw lists were
//    not ready yet, installGuard() defers wrapping to the first list opened, which then wraps
//    whatever is there (this wrapper, by then), making this wrapper the INNER one instead. A
//    single wrapped entry records exactly once either way (the draw list guard's own cut, if
//    any, only reorders itself against this entry's serialize-then-call-through, not against
//    another recorded entry, since there is only the one so far): no record is lost or
//    doubled. This stops being true once a second entry is wired that the draw list guard also
//    wraps (research/180's CUT table): the two wrappers' relative order then needs to be
//    decided on purpose, not left to whichever of install() and the first list open ran first.

namespace LLRHIReplayCapture
{
    // Called once, at the same point as LLRHI::DrawList::installGuard() (research/271 3.1):
    // after every hook the backend installs is set, before any other thread calls them. One
    // capture a run: once one has started, a later call (while it runs or after it ended) does
    // nothing; a call that could not start one (no file opened) leaves the next to try again.
    void install();

    // Called once per present_frame (research/271 3.2-4's frame boundary). A no-op unless a
    // capture is running, or a drawing thread stopped one (it is closed here then).
    void onFramePresented();

    // Called once, at LLRHIVKMirror::onGLShutdown (the viewer closing): flushes and closes a
    // capture that had not reached LLRHI_REPLAY_CAPTURE_FRAMES yet, so closing the viewer before
    // then does not lose what was recorded. A no-op unless a capture is running.
    void shutdown();
}

#endif // LL_LLRHIREPLAYCAPTURE_H
