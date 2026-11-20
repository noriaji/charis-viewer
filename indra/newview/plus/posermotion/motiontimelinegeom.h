/**
 * @file motiontimelinegeom.h
 * @brief The arithmetic of the timeline strip (research/357 section 2): time <-> x, the width of a second, the
 *        hit tests, the rounding of a segment's length while its right edge is dragged. No viewer dependency
 *        (motiontimelineview_test.cpp tests it alone); motiontimelineview.cpp draws with it.
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

// Coordinates: "content x" runs from the left end of the timeline (frame 0 at x = 0, before the margin); the view's
// own x is content x + TIMELINE_MARGIN_PX - scroll. y is measured from the TOP of the strip (the view flips it).
// Time is in frames of CLIP_FPS (30): the joined clip's frame 0 is the first key.
//
// Not linear on purpose: a segment is drawn at least TIMELINE_SEGMENT_MIN_PX wide (a 0.2 s segment would be 4 px at
// 20 px/s), and what comes after it moves right. xOfFrame / frameOfX are piecewise linear over the spans of Layout.

#ifndef PLUS_POSERMOTION_MOTIONTIMELINEGEOM_H
#define PLUS_POSERMOTION_MOTIONTIMELINEGEOM_H

#include "motiontimeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace plus::posermotion::tlgeom
{
    // ---- measures (px at 1080 high, like psstyle.h) ---------------------------------------------------------
    inline constexpr F32 PX_PER_SEC_MIN   = 20.f;
    inline constexpr F32 PX_PER_SEC_MAX   = 120.f;
    inline constexpr F32 SEGMENT_MIN_PX   = 18.f;   // a segment is drawn at least this wide
    inline constexpr F32 HOLD_MIN_PX      = 16.f;   // a hold too (the key's diamond would hide a shorter one)
    inline constexpr F32 EDGE_HIT_PX      = 12.f;   // the grab area of a segment's right edge (wider than it looks)
    inline constexpr F32 NEEDLE_HIT_PX    = 10.f;   // the grab area of the playhead's line in the track area
    inline constexpr F32 MARGIN_PX        = 12.f;   // left and right of the content (the first diamond is not cut)
    inline constexpr F32 RULER_H          = 18.f;
    inline constexpr F32 TRACK_TOP        = 22.f;   // from the strip's top: the bar of the segments
    inline constexpr F32 TRACK_H          = 26.f;
    inline constexpr F32 HOLD_H           = 12.f;   // a hold's bar, in the middle of the track
    inline constexpr F32 DIAMOND_R        = 7.f;
    inline constexpr F32 SCROLLBAR_TOP    = 56.f;
    inline constexpr F32 SCROLLBAR_H      = 3.f;
    inline constexpr F32 BAND_HEIGHT      = 62.f;   // the strip's height
    /// The most intervals (frames - 1) of the joined clip: 30 s.
    inline constexpr S32 MAX_INTERVALS    = TIMELINE_MAX_FRAMES - 1;
    inline constexpr S32 MIN_SMOOTH_INTERVALS = 6;   // SEGMENT_MIN_SEC * CLIP_FPS
    inline constexpr S32 MAX_SMOOTH_INTERVALS = 300; // SEGMENT_MAX_SEC * CLIP_FPS

    // ---- width of a second ----------------------------------------------------------------------------------
    /// clamp(width / total, 20, 120). A width or total that is not usable gives the widest (120).
    inline F32 pxPerSec(F32 usable_width, F64 total_sec)
    {
        if (!(usable_width > 0.f) || !(total_sec > 1e-6) || !std::isfinite(usable_width))
        {
            return PX_PER_SEC_MAX;
        }
        return std::clamp((F32)((F64)usable_width / total_sec), PX_PER_SEC_MIN, PX_PER_SEC_MAX);
    }

    /// The scroll kept inside 0 .. content - viewport (0 when the content fits).
    inline F32 clampScroll(F32 scroll, F32 content_width, F32 viewport_width)
    {
        const F32 most = std::max(0.f, content_width - viewport_width);
        return std::isfinite(scroll) ? std::clamp(scroll, 0.f, most) : 0.f;
    }

    /// The scroll that shows content x (at least `margin` from either side of the viewport); unchanged when it is
    /// shown already.
    inline F32 scrollToShow(F32 scroll, F32 content_x, F32 content_width, F32 viewport_width, F32 margin)
    {
        const F32 left  = content_x - margin;
        const F32 right = content_x + margin;
        if (left < scroll)
        {
            scroll = left;
        }
        else if (right > scroll + viewport_width)
        {
            scroll = right - viewport_width;
        }
        return clampScroll(scroll, content_width, viewport_width);
    }

    /// The step between the ruler's marks (seconds) that keeps them at least `min_gap_px` apart.
    inline F32 tickStepSec(F32 px_per_sec, F32 min_gap_px)
    {
        static constexpr F32 STEPS[] = { 0.1f, 0.2f, 0.5f, 1.f, 2.f, 5.f, 10.f };
        for (const F32 s : STEPS)
        {
            if (s * px_per_sec >= min_gap_px)
            {
                return s;
            }
        }
        return 10.f;
    }

    // ---- the spans of the strip ----------------------------------------------------------------------------
    enum class SpanKind : U8
    {
        Key,     // a key's place (no width; the diamond is centred on its x)
        Hold,    // the time a key's pose is kept
        Segment, // between a key and the next (the key's hold comes first)
    };
    struct Piece        // what the layout is built from, in the order of the strip
    {
        SpanKind kind   = SpanKind::Key;
        S32      index  = 0;    // the key's index (Key, Hold) or the segment's (Segment)
        S32      frames = 0;    // Hold, Segment: its length in frames (Key: 0)
        bool     exact  = false; // Segment: no minimum width (the one whose edge is dragged: its edge follows the mouse)
    };
    struct Span
    {
        SpanKind kind = SpanKind::Key;
        S32      index = 0;
        S32      f0 = 0, f1 = 0;   // frames of the joined clip
        F32      x0 = 0.f, x1 = 0.f;   // content x
    };

    class Layout
    {
    public:
        /// Rebuilds from pieces (the vector's memory is kept). A Segment is at least SEGMENT_MIN_PX wide.
        void build(const Piece* pieces, size_t count, F32 px_per_sec)
        {
            mPxPerSec = px_per_sec;
            mSpans.clear();
            const F32 per_frame = px_per_sec / (F32)CLIP_FPS;
            S32 f = 0;
            F32 x = 0.f;
            for (size_t i = 0; i < count; ++i)
            {
                const Piece& p = pieces[i];
                const S32    frames = std::max(0, p.frames);
                F32          w = per_frame * (F32)frames;
                if (p.kind == SpanKind::Segment)
                {
                    w = p.exact ? w : std::max(w, SEGMENT_MIN_PX);
                }
                else if (p.kind == SpanKind::Hold)
                {
                    w = std::max(w, HOLD_MIN_PX);
                }
                else
                {
                    w = 0.f;
                }
                mSpans.push_back(Span{ p.kind, p.index, f, f + (p.kind == SpanKind::Key ? 0 : frames), x, x + w });
                f += p.kind == SpanKind::Key ? 0 : frames;
                x += w;
            }
            mTotalFrames = f;
            mWidth       = x;
        }
        const std::vector<Span>& spans() const { return mSpans; }
        F32 pxPerSec() const { return mPxPerSec; }
        F32 pxPerFrame() const { return mPxPerSec / (F32)CLIP_FPS; }
        /// The frames from the first key to the last (the joined clip's frames - 1).
        S32 totalFrames() const { return mTotalFrames; }
        /// The strip's width without the margins.
        F32 width() const { return mWidth; }
        F32 contentWidth() const { return mWidth + 2.f * MARGIN_PX; }

        /// A frame's content x (clamped to the strip). A frame inside a stretched segment is spread over it.
        F32 xOfFrame(F64 frame) const
        {
            if (mSpans.empty())
            {
                return 0.f;
            }
            const F64 f = std::clamp(frame, 0.0, (F64)mTotalFrames);
            for (const Span& s : mSpans)
            {
                if (s.f1 > s.f0 && f <= (F64)s.f1)
                {
                    return s.x0 + (F32)((f - (F64)s.f0) / (F64)(s.f1 - s.f0)) * (s.x1 - s.x0);
                }
            }
            return mWidth;
        }
        /// The frame (fractional) at a content x, clamped to the strip.
        F64 frameOfX(F32 x) const
        {
            if (mSpans.empty() || !std::isfinite(x))
            {
                return 0.0;
            }
            if (x <= 0.f)
            {
                return 0.0;
            }
            for (const Span& s : mSpans)
            {
                if (s.f1 > s.f0 && x <= s.x1)
                {
                    const F32 w = s.x1 - s.x0;
                    return (F64)s.f0 + (w > 0.f ? (F64)((x - s.x0) / w) : 0.0) * (F64)(s.f1 - s.f0);
                }
            }
            return (F64)mTotalFrames;
        }
        /// The span of a kind and index (null if none).
        const Span* find(SpanKind kind, S32 index) const
        {
            for (const Span& s : mSpans)
            {
                if (s.kind == kind && s.index == index)
                {
                    return &s;
                }
            }
            return nullptr;
        }

    private:
        std::vector<Span> mSpans;
        F32               mPxPerSec    = PX_PER_SEC_MAX;
        S32               mTotalFrames = 0;
        F32               mWidth       = 0.f;
    };

    /// The width of a second at which the strip fits `usable` px, the minimum widths of the short segments and holds
    /// counted (they are wider than their time, so the plain usable / total can overshoot). Never under 20 px/s: then
    /// the strip scrolls. total_sec: the whole's length.
    inline F32 fitPxPerSec(const Piece* pieces, size_t count, F32 usable_width, F64 total_sec)
    {
        F32 pps = pxPerSec(usable_width, total_sec);
        if (!(usable_width > 0.f) || !std::isfinite(usable_width))
        {
            return pps;
        }
        for (int pass = 0; pass < 6 && pps > PX_PER_SEC_MIN; ++pass)
        {
            // what does not shrink with the width of a second: the pieces drawn at their minimum; the rest scales
            F32 fixed = 0.f;
            F64 free_frames = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const Piece& p = pieces[i];
                const F32 least = p.kind == SpanKind::Hold ? HOLD_MIN_PX : (p.kind == SpanKind::Segment && !p.exact ? SEGMENT_MIN_PX : 0.f);
                if (p.kind == SpanKind::Key)
                {
                    continue;
                }
                if (least > 0.f && pps / (F32)CLIP_FPS * (F32)p.frames < least)
                {
                    fixed += least;
                }
                else
                {
                    free_frames += (F64)std::max(0, p.frames);
                }
            }
            if (fixed + (F32)(free_frames / CLIP_FPS) * pps <= usable_width + 0.01f)
            {
                break;
            }
            if (!(free_frames > 0.0) || usable_width - fixed <= 0.f)
            {
                pps = PX_PER_SEC_MIN;
                break;
            }
            pps = std::clamp((F32)((F64)(usable_width - fixed) / (free_frames / CLIP_FPS)), PX_PER_SEC_MIN, pps);
        }
        return pps;
    }

    /// The marks of the ruler: where (content x) and what they say. rebuild() follows the layout; the words are made
    /// again only when they can differ (the step changed, or more marks than words), and it says so.
    class TickSet
    {
    public:
        /// Returns true when the words were made again (the caller then makes what it draws them with again).
        bool rebuild(const Layout& layout, F32 min_gap_px)
        {
            const F32 step  = tickStepSec(layout.pxPerSec(), min_gap_px);
            const F64 total = (F64)layout.totalFrames() / (F64)CLIP_FPS;
            const S32 count = (S32)std::floor(total / (F64)step + 1e-6) + 1;
            if (step != mStep)
            {
                mText.clear();          // the words depend on the step: all are made again
            }
            const bool again = step != mStep || (size_t)count > mText.size();
            mStep  = step;
            mCount = count;
            mX.resize((size_t)count);
            if (mText.size() < (size_t)count)
            {
                mText.resize((size_t)count);
            }
            for (S32 i = 0; i < count; ++i)
            {
                mX[(size_t)i] = layout.xOfFrame(std::round((F64)step * i * CLIP_FPS));
                if (again)
                {
                    mText[(size_t)i] = label(step, i);
                }
            }
            return again;
        }
        S32  count() const { return mCount; }
        F32  step() const { return mStep; }
        F32  x(S32 i) const { return mX[(size_t)i]; }
        const std::string& text(S32 i) const { return mText[(size_t)i]; }

        /// A mark's words: whole seconds, or tenths when the marks are closer than a second.
        static std::string label(F32 step, S32 index)
        {
            char buf[24];
            if (step >= 1.f)
            {
                std::snprintf(buf, sizeof(buf), "%d", (int)std::lround((F64)step * index));
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "%.1f", (F64)step * index);
            }
            return buf;
        }

    private:
        std::vector<F32>         mX;
        std::vector<std::string> mText;
        S32                      mCount = 0;
        F32                      mStep  = 0.f;
    };

    // ---- hit tests ----------------------------------------------------------------------------------------
    enum class HitKind : U8
    {
        None,
        Ruler,       // the ruler: a press starts the playhead's drag (and puts it there)
        Needle,      // the playhead's line, and its handle on the ruler: a press starts the drag, the playhead stays
        SegmentEdge, // the right edge of segment `index`
        Segment,     // the body of segment `index` (no action, for the tool tip)
        Hold,        // the hold of key `index` (no action, for the tool tip)
        Key,         // the diamond of key `index` (no action, for the tool tip)
    };
    struct Hit
    {
        HitKind kind  = HitKind::None;
        S32     index = -1;
        bool operator==(const Hit&) const = default;
    };

    /// What is at (content x, y from the strip's top). The playhead is at needle_x (content x). Order: in the ruler,
    /// everything is the playhead (a press near its line does not move it); in the track area a segment's right edge wins over the playhead's line, and
    /// that over the bodies. Segments that are 18 px wide at least leave their edges' grab areas (12 px) apart.
    inline Hit hitTest(const Layout& layout, F32 x, F32 y, F32 needle_x)
    {
        if (y >= 0.f && y < RULER_H)
        {
            // the handle on the ruler is the playhead's line too: pressing it does not move the playhead
            return { std::fabs(x - needle_x) <= NEEDLE_HIT_PX * 0.5f ? HitKind::Needle : HitKind::Ruler, -1 };
        }
        if (y < TRACK_TOP - 2.f || y > TRACK_TOP + TRACK_H + 2.f)
        {
            return {};
        }
        for (const Span& s : layout.spans())
        {
            if (s.kind == SpanKind::Segment && std::fabs(x - s.x1) <= EDGE_HIT_PX * 0.5f)
            {
                return { HitKind::SegmentEdge, s.index };
            }
        }
        if (std::fabs(x - needle_x) <= NEEDLE_HIT_PX * 0.5f)
        {
            return { HitKind::Needle, -1 };
        }
        for (const Span& s : layout.spans())
        {
            if (s.kind == SpanKind::Key && std::fabs(x - s.x0) <= DIAMOND_R)
            {
                return { HitKind::Key, s.index };
            }
        }
        for (const Span& s : layout.spans())
        {
            if (s.kind != SpanKind::Key && x >= s.x0 && x < s.x1)
            {
                return { s.kind == SpanKind::Segment ? HitKind::Segment : HitKind::Hold, s.index };
            }
        }
        return {};
    }

    // ---- the drag of a segment's right edge -----------------------------------------------------------------
    struct EdgeDrag
    {
        S32  frames   = 0;      // the length to show now (intervals between the two keys); never past the limit
        S32  tokens   = 0;      // Generate: 6..16; else 0
        F32  seconds  = 0.f;    // Smooth, Linear: frames / 30 (what setSeconds takes); Generate: the length in seconds
        bool rejected = false;  // the whole would pass 30 s (the mark: the edge turns to the warning colour); nothing is committed
        bool changed  = false;  // frames differ from where the drag began
    };

    /// The length of a segment while its right edge is dragged by dx_px from where it was grabbed.
    /// frames0: its length then (intervals); total_intervals: the whole's length then (frames - 1).
    /// Generate: the tokens 6..16 (4 frames each), nearest to the wish; Smooth, Linear: any 0.2..10 s, to the frame.
    /// A wish past what 30 s leaves is shown at the most that fits and rejected (the release then does nothing).
    inline EdgeDrag resolveEdgeDrag(SegMethod method, S32 frames0, F32 dx_px, F32 px_per_frame, S32 total_intervals)
    {
        EdgeDrag out;
        out.frames = frames0;
        const S32 allowed = std::max(0, MAX_INTERVALS - (total_intervals - frames0));   // the most this segment may be
        if (!(px_per_frame > 0.f) || !std::isfinite(dx_px))
        {
            return out;
        }
        const F64 wish = (F64)frames0 + (F64)dx_px / (F64)px_per_frame;
        if (method == SegMethod::Generate)
        {
            // frames of t tokens: timelineFramesOfTokens(t) - 1 = 4 t - 7 (the intervals)
            S32 t = (S32)std::lround((wish + 7.0) / 4.0);
            t = std::clamp(t, TIMELINE_TOKENS_MIN, TIMELINE_TOKENS_MAX);
            out.rejected = (t * 4 - 7) > allowed;
            while (t > TIMELINE_TOKENS_MIN && (t * 4 - 7) > allowed)
            {
                --t;
            }
            if ((t * 4 - 7) > allowed)
            {
                out.rejected = true;
                out.changed  = false;
                out.tokens   = 0;
                out.seconds  = (F32)frames0 / (F32)CLIP_FPS;
                return out;   // not even the shortest fits
            }
            out.tokens = t;
            out.frames = t * 4 - 7;
        }
        else
        {
            const S32 w = (S32)std::lround(std::clamp(wish, (F64)MIN_SMOOTH_INTERVALS, (F64)MAX_SMOOTH_INTERVALS));
            if (w > allowed)
            {
                out.rejected = true;
                if (allowed < MIN_SMOOTH_INTERVALS)
                {
                    out.seconds = (F32)frames0 / (F32)CLIP_FPS;
                    return out;
                }
                out.frames = allowed;
            }
            else
            {
                out.frames = w;
            }
        }
        out.seconds = (F32)out.frames / (F32)CLIP_FPS;
        out.changed = out.frames != frames0;
        return out;
    }

    /// The seconds of the playhead for a content x: the frame, to a whole frame, in seconds.
    inline F64 seekSecOfX(const Layout& layout, F32 x)
    {
        return std::round(layout.frameOfX(x)) / (F64)CLIP_FPS;
    }

    // ---- the drag, as a machine without the viewer -------------------------------------------------------------
    // The view feeds it what the mouse and the timeline do and does what it says. Every way out of a drag is release()
    // (the user lets go: the only one that can say something), cancel() (Esc, focus or capture lost, hidden, disabled),
    // or onTimeline() / setEditable() finding that the drag cannot go on. After any of them the state is Idle, and a
    // second one says nothing (no double fire).
    enum class DragState : U8
    {
        Idle,
        Playhead,
        Edge,
    };
    struct DragOutcome
    {
        enum class Kind : U8 { None, Seek, Resize };
        Kind      kind     = Kind::None;
        F64       seek_sec = 0.0;           // Seek
        PairId    pair;                     // Resize: the segment, its method and the length
        SegMethod method   = SegMethod::Generate;
        EdgeDrag  edge;
    };
    // ---- where the keyboard focus goes when a drag ends ----------------------------------------------------------
    // The strip takes the focus for the drag (Esc) and gives it back. LLUICtrl::setFocus(false) would leave it with nobody
    // (the floater then turns inactive), and setFocus(true) on a holder that is an ancestor of the strip does nothing (it
    // sees its child having the focus already), nor does it on a holder that has become disabled.
    enum class FocusReturn : U8
    {
        Previous,        // previous->setFocus(true)
        PreviousDirect,  // the focus manager is told directly (the previous holder is an ancestor: the floater, a panel)
        Floater,         // the previous holder is gone, hidden or disabled: the floater around the strip (else nobody)
    };
    inline FocusReturn chooseFocusReturn(bool previous_alive, bool previous_usable, bool previous_is_ancestor)
    {
        if (!previous_alive || !previous_usable)
        {
            return FocusReturn::Floater;
        }
        return previous_is_ancestor ? FocusReturn::PreviousDirect : FocusReturn::Previous;
    }

    /// The share of the scroll mark: where it is along its track, kept inside 0..1 (the scroll may be past the end while an
    /// edge is dragged and the strip shrinks).
    inline F32 scrollShare(F32 scroll, F32 content_width, F32 viewport_width)
    {
        const F32 range = content_width - viewport_width;
        return range > 0.f && std::isfinite(scroll) ? std::clamp(scroll / range, 0.f, 1.f) : 0.f;
    }

    /// What the timeline now says of the segment an edge drag holds (the view looks it up by pair).
    struct SegmentNow
    {
        bool      found    = false;
        S32       index    = -1;
        SegMethod method   = SegMethod::Generate;
        S32       frames   = 0;
        bool      editable = false;   // not queued or running, and the view is editable
    };
    class DragMachine
    {
    public:
        DragState state() const { return mState; }
        bool      dragging() const { return mState != DragState::Idle; }
        /// Where the playhead was last put by the drag (kept after the drag ends, until the next one).
        F64       playheadSec() const { return mSec; }
        /// The segment of an edge drag (index in the view's list; kept after it ends) and the length being shown.
        S32             segmentIndex() const { return mSeg; }
        const EdgeDrag& edge() const { return mEdge; }

        // ---- the playhead ----
        /// A press on the ruler (from_ruler: the playhead goes to sec and a Seek is said at once) or on the playhead's
        /// line (it stays where it is until the mouse moves to another frame). current_sec: where it is now.
        DragOutcome pressPlayhead(F64 current_sec, F64 sec, bool from_ruler)
        {
            mState     = DragState::Playhead;
            mSec       = current_sec;
            mLastFrame = from_ruler ? -1 : frameOf(current_sec);
            return movePlayhead(sec);
        }
        /// The mouse is at sec: a Seek when the frame is another than the last said.
        DragOutcome movePlayhead(F64 sec)
        {
            DragOutcome out;
            if (mState != DragState::Playhead)
            {
                return out;
            }
            mSec = sec;
            if (frameOf(sec) != mLastFrame)
            {
                mLastFrame   = frameOf(sec);
                out.kind     = DragOutcome::Kind::Seek;
                out.seek_sec = sec;
            }
            return out;
        }

        // ---- a segment's right edge ----
        void pressEdge(S32 segment_index, PairId pair, SegMethod method, S32 frames0, S32 total_intervals0, F32 px_per_frame, F32 x)
        {
            mState   = DragState::Edge;
            mSeg     = segment_index;
            mPair    = pair;
            mMethod  = method;
            mFrames0 = frames0;
            mTotal0  = total_intervals0;
            mPpf     = px_per_frame;
            mX0      = x;
            mEdge    = resolveEdgeDrag(method, frames0, 0.f, px_per_frame, total_intervals0);
        }
        /// The mouse is at x. True when what is shown changed (the length or the refusal).
        bool moveEdge(F32 x)
        {
            if (mState != DragState::Edge)
            {
                return false;
            }
            const EdgeDrag now = resolveEdgeDrag(mMethod, mFrames0, x - mX0, mPpf, mTotal0);
            const bool changed = now.frames != mEdge.frames || now.rejected != mEdge.rejected;
            mEdge = now;
            return changed;
        }

        // ---- the ways out ----
        /// The user lets go at sec (the playhead's position; not used for an edge). Playhead: a Seek when the frame is
        /// another than the last said. Edge: a Resize when the length changed and was not refused.
        DragOutcome release(F64 sec)
        {
            DragOutcome out;
            if (mState == DragState::Playhead)
            {
                out = movePlayhead(sec);
            }
            else if (mState == DragState::Edge && mEdge.changed && !mEdge.rejected)
            {
                out.kind   = DragOutcome::Kind::Resize;
                out.pair   = mPair;
                out.method = mMethod;
                out.edge   = mEdge;
            }
            mState = DragState::Idle;
            return out;
        }
        /// Everything that is not a release. True when there was a drag.
        bool cancel()
        {
            const bool was = mState != DragState::Idle;
            mState = DragState::Idle;
            return was;
        }
        /// A new timeline came. An edge drag goes on only if its segment is still there (found), can be changed
        /// (editable) and is as it was: the method, its length and the whole's length. True when the drag ended.
        bool onTimeline(const SegmentNow& now, S32 total_intervals)
        {
            if (mState != DragState::Edge)
            {
                return false;
            }
            if (!now.found || !now.editable || now.method != mMethod || now.frames != mFrames0 || total_intervals != mTotal0)
            {
                mState = DragState::Idle;
                return true;
            }
            mSeg = now.index;
            return false;
        }
        /// editable == false ends an edge drag (the playhead's goes on). True when one ended.
        bool setEditable(bool editable)
        {
            if (!editable && mState == DragState::Edge)
            {
                mState = DragState::Idle;
                return true;
            }
            return false;
        }
        PairId pair() const { return mPair; }

    private:
        static S32 frameOf(F64 sec) { return (S32)std::lround(sec * (F64)CLIP_FPS); }

        DragState mState     = DragState::Idle;
        F64       mSec       = 0.0;
        S32       mLastFrame = -1;
        S32       mSeg       = -1;
        PairId    mPair;
        SegMethod mMethod    = SegMethod::Generate;
        S32       mFrames0   = 0;
        S32       mTotal0    = 0;
        F32       mPpf       = 0.f;
        F32       mX0        = 0.f;
        EdgeDrag  mEdge;
    };
} // namespace plus::posermotion::tlgeom

#endif // PLUS_POSERMOTION_MOTIONTIMELINEGEOM_H
