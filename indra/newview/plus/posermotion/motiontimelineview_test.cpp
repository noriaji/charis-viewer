/**
 * @file motiontimelineview_test.cpp
 * @brief Standalone test of motiontimelinegeom.h (the timeline strip's arithmetic, research/357 tl-view).
 *        Built by tools/motionbricks-eval/build_timelineview_test.bat, not by the viewer.
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

// selftest: tools/motionbricks-eval/build_timelineview_test.bat

#include "linden_common.h"

#include "motiontimelinegeom.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace plus::posermotion;
using namespace plus::posermotion::tlgeom;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool closeTo(F64 a, F64 b, F64 eps = 1e-3) { return std::fabs(a - b) <= eps; }

// keys: K0 [hold h0] S0 K1 [hold h1] S1 K2 ... (the strip's order: key, hold, segment)
static std::vector<Piece> piecesOf(const std::vector<S32>& holds, const std::vector<S32>& segs)
{
    std::vector<Piece> out;
    for (size_t i = 0; i < holds.size(); ++i)
    {
        out.push_back({ SpanKind::Key, (S32)i, 0 });
        if (holds[i] > 0)
        {
            out.push_back({ SpanKind::Hold, (S32)i, holds[i] });
        }
        if (i < segs.size())
        {
            out.push_back({ SpanKind::Segment, (S32)i, segs[i] });
        }
    }
    return out;
}

static void testPxPerSec()
{
    CHECK(closeTo(pxPerSec(480.f, 10.0), 48.0));
    CHECK(closeTo(pxPerSec(480.f, 30.0), 20.0));     // 16 -> clamped up: the strip scrolls
    CHECK(closeTo(pxPerSec(480.f, 2.0), 120.0));     // 240 -> clamped down
    CHECK(closeTo(pxPerSec(480.f, 4.0), 120.0));     // exactly the top
    CHECK(closeTo(pxPerSec(480.f, 0.0), 120.0));     // an empty timeline
    CHECK(closeTo(pxPerSec(0.f, 5.0), 120.0));
    CHECK(closeTo(pxPerSec(-5.f, 5.0), 120.0));
    CHECK(closeTo(pxPerSec(std::numeric_limits<F32>::quiet_NaN(), 5.0), 120.0));
    CHECK(closeTo(pxPerSec(480.f, std::numeric_limits<F64>::quiet_NaN()), 120.0));
}

static void testScroll()
{
    CHECK(closeTo(clampScroll(50.f, 400.f, 500.f), 0.0));     // fits: no scroll
    CHECK(closeTo(clampScroll(50.f, 900.f, 500.f), 50.0));
    CHECK(closeTo(clampScroll(-10.f, 900.f, 500.f), 0.0));
    CHECK(closeTo(clampScroll(999.f, 900.f, 500.f), 400.0));  // the end: content - viewport
    CHECK(closeTo(clampScroll(std::numeric_limits<F32>::quiet_NaN(), 900.f, 500.f), 0.0));
    CHECK(closeTo(scrollToShow(0.f, 300.f, 900.f, 500.f, 20.f), 0.0));      // shown already
    CHECK(closeTo(scrollToShow(0.f, 700.f, 900.f, 500.f, 20.f), 220.0));    // to the right: x + margin at the edge
    CHECK(closeTo(scrollToShow(300.f, 100.f, 900.f, 500.f, 20.f), 80.0));   // to the left
    CHECK(closeTo(scrollToShow(0.f, 890.f, 900.f, 500.f, 20.f), 400.0));    // clamped at the end
}

static void testTicks()
{
    CHECK(closeTo(tickStepSec(120.f, 40.f), 0.5));
    CHECK(closeTo(tickStepSec(48.f, 40.f), 1.0));
    CHECK(closeTo(tickStepSec(20.f, 40.f), 2.0));
    CHECK(closeTo(tickStepSec(1.f, 40.f), 10.0));     // nothing fits: the biggest
}

static void testLayoutLinear()
{
    // two keys, a 3 s segment, 60 px/s: 90 frames -> 180 px
    const std::vector<Piece> p = piecesOf({ 0, 0 }, { 90 });
    Layout l;
    l.build(p.data(), p.size(), 60.f);
    CHECK(l.totalFrames() == 90);
    CHECK(closeTo(l.width(), 180.0));
    CHECK(closeTo(l.contentWidth(), 180.0 + 2 * MARGIN_PX));
    CHECK(closeTo(l.xOfFrame(0), 0.0));
    CHECK(closeTo(l.xOfFrame(45), 90.0));
    CHECK(closeTo(l.xOfFrame(90), 180.0));
    CHECK(closeTo(l.xOfFrame(1000), 180.0));          // clamped
    CHECK(closeTo(l.xOfFrame(-5), 0.0));
    CHECK(closeTo(l.frameOfX(90.f), 45.0));
    CHECK(closeTo(l.frameOfX(-30.f), 0.0));
    CHECK(closeTo(l.frameOfX(500.f), 90.0));
    CHECK(closeTo(seekSecOfX(l, 91.f), 46.0 / 30.0));  // to a whole frame (45.5 -> 46: round half away)
    CHECK(l.find(SpanKind::Key, 1) && closeTo(l.find(SpanKind::Key, 1)->x0, 180.0));
    CHECK(l.find(SpanKind::Segment, 0) != nullptr);
    CHECK(l.find(SpanKind::Segment, 3) == nullptr);
}

static void testLayoutHoldAndMinWidth()
{
    // K0 hold 30, S0 (0.2 s = 6 frames), K1 hold 15, S1 (30 frames), K2   at 20 px/s
    const std::vector<Piece> p = piecesOf({ 30, 15, 0 }, { 6, 30 });
    Layout l;
    l.build(p.data(), p.size(), 20.f);
    // hold 1 s = 20 px; S0 natural 4 px -> 18; hold 0.5 s = 10 px -> 16 (the least of a hold); S1 1 s = 20
    CHECK(l.totalFrames() == 30 + 6 + 15 + 30);
    CHECK(closeTo(l.width(), 20 + 18 + HOLD_MIN_PX + 20));
    const Span* s0 = l.find(SpanKind::Segment, 0);
    CHECK(s0 && closeTo(s0->x0, 20.0) && closeTo(s0->x1, 38.0));
    CHECK(s0 && s0->f0 == 30 && s0->f1 == 36);
    CHECK(s0 && closeTo(s0->x1 - s0->x0, SEGMENT_MIN_PX));
    // inside the stretched segment, frames spread over the 18 px (monotonic)
    F32 last = -1.f;
    for (S32 f = 30; f <= 36; ++f)
    {
        const F32 x = l.xOfFrame(f);
        CHECK(x >= last);
        last = x;
    }
    CHECK(closeTo(l.xOfFrame(33), 29.0));
    CHECK(closeTo(l.frameOfX(29.f), 33.0));
    // after it, the key 1's place is where the stretched segment ends
    CHECK(closeTo(l.find(SpanKind::Key, 1)->x0, 38.0));
    // round trip over the whole strip
    for (S32 f = 0; f <= l.totalFrames(); ++f)
    {
        CHECK(closeTo(l.frameOfX(l.xOfFrame(f)), (F64)f, 1e-2));
    }
    CHECK(l.find(SpanKind::Hold, 2) == nullptr);   // no hold: no span
}

static void testLayoutEmpty()
{
    Layout l;
    l.build(nullptr, 0, 60.f);
    CHECK(l.totalFrames() == 0 && closeTo(l.width(), 0.0));
    CHECK(closeTo(l.xOfFrame(5), 0.0) && closeTo(l.frameOfX(5.f), 0.0));
    const std::vector<Piece> one = { { SpanKind::Key, 0, 0 } };   // one key: a point
    l.build(one.data(), one.size(), 60.f);
    CHECK(l.totalFrames() == 0 && closeTo(l.width(), 0.0));
    CHECK(closeTo(l.xOfFrame(10), 0.0));
}

static void testHit()
{
    const std::vector<Piece> p = piecesOf({ 0, 0, 0 }, { 60, 60 });
    Layout l;
    l.build(p.data(), p.size(), 60.f);   // S0: 0..120, S1: 120..240; keys at 0, 120, 240
    const F32 mid = TRACK_TOP + TRACK_H * 0.5f;
    CHECK(hitTest(l, 50.f, 5.f, 10.f).kind == HitKind::Ruler);
    CHECK(hitTest(l, 5000.f, 0.f, 10.f).kind == HitKind::Ruler);      // anywhere in the ruler
    CHECK(hitTest(l, 50.f, RULER_H + 0.5f, 10.f).kind == HitKind::None);   // the gap between
    CHECK(hitTest(l, 50.f, BAND_HEIGHT - 1.f, 10.f).kind == HitKind::None);
    Hit h = hitTest(l, 60.f, mid, 10.f);
    CHECK(h.kind == HitKind::Segment && h.index == 0);
    h = hitTest(l, 120.f - 5.f, mid, 10.f);                           // within 6 px of S0's right edge
    CHECK(h.kind == HitKind::SegmentEdge && h.index == 0);
    h = hitTest(l, 120.f + 6.f, mid, 10.f);                           // the 12 px area: +-6
    CHECK(h.kind == HitKind::SegmentEdge && h.index == 0);
    h = hitTest(l, 120.f + 6.5f, mid, 10.f);                          // past the edge's area, on the diamond of the key
    CHECK(h.kind == HitKind::Key && h.index == 1);
    h = hitTest(l, 120.f + 8.f, mid, 10.f);
    CHECK(h.kind == HitKind::Segment && h.index == 1);
    h = hitTest(l, 240.f, mid, 10.f);
    CHECK(h.kind == HitKind::SegmentEdge && h.index == 1);            // the last segment's edge too
    CHECK(hitTest(l, 70.f, mid, 70.f).kind == HitKind::Needle);       // the line, inside a body
    CHECK(hitTest(l, 74.f, mid, 70.f).kind == HitKind::Needle);
    CHECK(hitTest(l, 76.f, mid, 70.f).kind == HitKind::Segment);
    // the needle on an edge: the edge wins
    CHECK(hitTest(l, 120.f, mid, 120.f).kind == HitKind::SegmentEdge);
    // the key at 0 (no segment edge there): the diamond
    h = hitTest(l, 3.f, mid, 100.f);
    CHECK(h.kind == HitKind::Key && h.index == 0);
    // out of the strip to the right: nothing
    CHECK(hitTest(l, 400.f, mid, 10.f).kind == HitKind::None);
    // the narrowest segments (18 px) keep their two edge areas apart
    const std::vector<Piece> q = piecesOf({ 0, 0, 0 }, { 6, 6 });
    l.build(q.data(), q.size(), 20.f);                                // 18 px each: edges at 18 and 36
    h = hitTest(l, 18.f + 6.f, mid, 500.f);
    CHECK(h.kind == HitKind::SegmentEdge && h.index == 0);
    h = hitTest(l, 36.f - 6.f, mid, 500.f);
    CHECK(h.kind == HitKind::SegmentEdge && h.index == 1);
    h = hitTest(l, 27.f, mid, 500.f);                                 // between the areas: S1's body
    CHECK(h.kind == HitKind::Segment && h.index == 1);
}

static void testHoldHit()
{
    const std::vector<Piece> p = piecesOf({ 60, 0 }, { 30 });          // K0 + 2 s hold, then S0
    Layout l;
    l.build(p.data(), p.size(), 60.f);                                 // hold 0..120, S0 120..180
    const F32 mid = TRACK_TOP + TRACK_H * 0.5f;
    const Hit h = hitTest(l, 60.f, mid, 500.f);
    CHECK(h.kind == HitKind::Hold && h.index == 0);
}

static void testDragGenerate()
{
    // 10 tokens = 33 intervals. 60 px/s = 2 px/frame. A whole of 100 intervals.
    EdgeDrag d = resolveEdgeDrag(SegMethod::Generate, 33, 0.f, 2.f, 100);
    CHECK(d.tokens == 10 && d.frames == 33 && !d.changed && !d.rejected);
    d = resolveEdgeDrag(SegMethod::Generate, 33, 8.f, 2.f, 100);       // +4 frames: 11 tokens (37)
    CHECK(d.tokens == 11 && d.frames == 37 && d.changed && !d.rejected);
    d = resolveEdgeDrag(SegMethod::Generate, 33, 3.f, 2.f, 100);       // +1.5: 34.5 -> (34.5+7)/4 = 10.4 -> 10
    CHECK(d.tokens == 10 && !d.changed);
    d = resolveEdgeDrag(SegMethod::Generate, 33, 5.f, 2.f, 100);       // +2.5: 35.5 -> 10.6 -> 11
    CHECK(d.tokens == 11);
    d = resolveEdgeDrag(SegMethod::Generate, 33, 1000.f, 2.f, 100);    // far right: 16 tokens, 57 intervals
    CHECK(d.tokens == 16 && d.frames == 57 && !d.rejected);
    d = resolveEdgeDrag(SegMethod::Generate, 33, -1000.f, 2.f, 100);   // far left: 6 tokens, 17 intervals
    CHECK(d.tokens == 6 && d.frames == 17 && d.changed);
    for (S32 t = TIMELINE_TOKENS_MIN; t <= TIMELINE_TOKENS_MAX; ++t)   // every step is a whole token
    {
        d = resolveEdgeDrag(SegMethod::Generate, 33, (F32)((t * 4 - 7) - 33) * 2.f, 2.f, 100);
        CHECK(d.tokens == t && d.frames == t * 4 - 7);
        CHECK(d.frames + 1 == timelineFramesOfTokens(t) );
    }
    d = resolveEdgeDrag(SegMethod::Generate, 33, 5.f, 2.f, 100);
    CHECK(closeTo(d.seconds, d.frames / 30.0));
}

static void testDragSmooth()
{
    // 1 s = 30 intervals; 60 px/s = 2 px/frame
    EdgeDrag d = resolveEdgeDrag(SegMethod::Smooth, 30, 10.f, 2.f, 90);   // +5 frames
    CHECK(d.frames == 35 && d.changed && !d.rejected && d.tokens == 0);
    CHECK(closeTo(d.seconds, 35.0 / 30.0));
    d = resolveEdgeDrag(SegMethod::Linear, 30, 1.f, 2.f, 90);             // +0.5: 30.5 -> 31 (lround half away)
    CHECK(d.frames == 31);
    d = resolveEdgeDrag(SegMethod::Smooth, 30, -1000.f, 2.f, 90);         // 0.2 s at least
    CHECK(d.frames == MIN_SMOOTH_INTERVALS && d.changed);
    CHECK(closeTo(d.seconds, SEGMENT_MIN_SEC));
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 10000.f, 2.f, 30);         // 10 s at the most (the whole is only this segment)
    CHECK(d.frames == MAX_SMOOTH_INTERVALS && !d.rejected);
    CHECK(closeTo(d.seconds, SEGMENT_MAX_SEC));
}

static void testDragLimit()
{
    // the whole is 880 intervals (29.3 s): this segment 30 of it, so it may be 50 at the most
    EdgeDrag d = resolveEdgeDrag(SegMethod::Smooth, 30, 2000.f, 2.f, 880);
    CHECK(d.rejected && d.frames == 50 && d.changed);                    // shown at the most that fits, and refused
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 20.f, 2.f, 880);           // +10 frames: 40, fits
    CHECK(!d.rejected && d.frames == 40);
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 40.f, 2.f, 880);           // exactly 50
    CHECK(!d.rejected && d.frames == 50);
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 42.f, 2.f, 880);           // 51
    CHECK(d.rejected && d.frames == 50);
    // exactly 900 whole: no room to grow
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 2.f, 2.f, 900);
    CHECK(d.rejected && d.frames == 30 && !d.changed);
    // Generate: 33 of 880 -> 53 at most: 14 tokens (49) fits, 15 (53) fits, 16 (57) does not
    d = resolveEdgeDrag(SegMethod::Generate, 33, 1000.f, 2.f, 880);
    CHECK(d.rejected && d.tokens == 15 && d.frames == 53);
    d = resolveEdgeDrag(SegMethod::Generate, 33, 2.f * (49 - 33), 2.f, 880);
    CHECK(!d.rejected && d.tokens == 14);
    // a state that cannot happen in the model (the others alone are past the limit): even the shortest does not fit,
    // the segment stays as it is
    d = resolveEdgeDrag(SegMethod::Generate, 20, -1000.f, 2.f, 900 + 20 - 10);           // allowed = 900 - (910 - 20) = 10 < 17
    CHECK(d.rejected && !d.changed && d.frames == 20 && d.tokens == 0);
}

static void testDragBadInput()
{
    EdgeDrag d = resolveEdgeDrag(SegMethod::Smooth, 30, std::numeric_limits<F32>::quiet_NaN(), 2.f, 90);
    CHECK(!d.changed && d.frames == 30);
    d = resolveEdgeDrag(SegMethod::Smooth, 30, 10.f, 0.f, 90);
    CHECK(!d.changed && d.frames == 30);
    d = resolveEdgeDrag(SegMethod::Generate, 33, std::numeric_limits<F32>::infinity(), 2.f, 90);
    CHECK(!d.changed && d.frames == 33);
}

static void testTicks44()
{
    // the real minimum gap of the strip is 44 px
    CHECK(closeTo(tickStepSec(120.f, 44.f), 0.5));
    CHECK(closeTo(tickStepSec(88.f, 44.f), 0.5));     // 0.5 * 88 = 44: exactly enough
    CHECK(closeTo(tickStepSec(87.f, 44.f), 1.0));
    CHECK(closeTo(tickStepSec(22.f, 44.f), 2.0));
    CHECK(closeTo(tickStepSec(21.f, 44.f), 5.0));     // 2 * 21 = 42 < 44: the next step (differs from the 40 px answer)
    CHECK(closeTo(tickStepSec(20.f, 44.f), 5.0));
}

static void testHoldMinAndExact()
{
    // K0 hold 3 frames (0.1 s = 2 px at 20 px/s -> 16), S0 6 frames (-> 18), K1
    const std::vector<Piece> p = piecesOf({ 3, 0 }, { 6 });
    Layout l;
    l.build(p.data(), p.size(), 20.f);
    CHECK(closeTo(l.find(SpanKind::Hold, 0)->x1 - l.find(SpanKind::Hold, 0)->x0, HOLD_MIN_PX));
    CHECK(closeTo(l.width(), HOLD_MIN_PX + SEGMENT_MIN_PX));
    // the segment that is dragged is exact: 6 frames at 20 px/s = 4 px
    std::vector<Piece> q = p;
    for (Piece& piece : q)
    {
        piece.exact = piece.kind == SpanKind::Segment;
    }
    l.build(q.data(), q.size(), 20.f);
    CHECK(closeTo(l.find(SpanKind::Segment, 0)->x1 - l.find(SpanKind::Segment, 0)->x0, 4.0));
    // a stretched segment: seekSecOfX inside it spreads its frames over the 18 px
    l.build(p.data(), p.size(), 20.f);
    const Span* s = l.find(SpanKind::Segment, 0);
    CHECK(s != nullptr);
    CHECK(closeTo(seekSecOfX(l, (s->x0 + s->x1) * 0.5f), (3 + 3) / 30.0));          // the middle: frame 3 of 6, after the hold
    CHECK(closeTo(seekSecOfX(l, s->x1 - 0.01f), (3 + 6) / 30.0));                  // the end of the segment
}

static void testFit()
{
    // one 0.2 s segment between two keys: 120 px/s (the cap); the strip is 24 px
    std::vector<Piece> p = piecesOf({ 0, 0 }, { 6 });
    CHECK(closeTo(fitPxPerSec(p.data(), p.size(), 100.f, 0.2), 120.0));
    // a 3 s segment and five 0.2 s ones (18 px at least each) in 300 px: the plain 300 / 4 s = 75 px/s is too wide by the
    // 15 px the short ones are stretched; the fit is (300 - 90) / 3 s = 70 and the strip is as wide as the room
    p = piecesOf({ 0, 0, 0, 0, 0, 0, 0 }, { 90, 6, 6, 6, 6, 6 });
    const F32 plain = pxPerSec(300.f, 4.0);
    const F32 fit   = fitPxPerSec(p.data(), p.size(), 300.f, 4.0);
    Layout l;
    l.build(p.data(), p.size(), fit);
    CHECK(closeTo(plain, 75.0) && closeTo(fit, 70.0));
    CHECK(l.width() <= 300.f + 0.02f && l.width() > 299.f);
    // the stretch set grows while the second narrows: still fits
    p = piecesOf({ 0, 0, 0, 0 }, { 60, 12, 20 });
    const F32 fit2 = fitPxPerSec(p.data(), p.size(), 200.f, 3.1);
    l.build(p.data(), p.size(), fit2);
    CHECK(l.width() <= 200.f + 0.02f);
    // everything at its minimum does not fit: 20 px/s and the strip scrolls
    std::vector<Piece> many;
    for (S32 i = 0; i < 16; ++i)
    {
        many.push_back({ SpanKind::Key, i, 0, false });
        if (i < 15)
        {
            many.push_back({ SpanKind::Segment, i, 6, false });
        }
    }
    CHECK(closeTo(fitPxPerSec(many.data(), many.size(), 100.f, 3.0), PX_PER_SEC_MIN));
    // nothing stretched: the plain value
    p = piecesOf({ 0, 0 }, { 150 });
    CHECK(closeTo(fitPxPerSec(p.data(), p.size(), 300.f, 5.0), 60.0));
    // a bad room: the plain answer
    CHECK(closeTo(fitPxPerSec(p.data(), p.size(), 0.f, 5.0), 120.0));
}

static void testTickSet()
{
    TickSet t;
    std::vector<Piece> p = piecesOf({ 0, 0 }, { 300 });          // 10 s
    Layout l;
    l.build(p.data(), p.size(), 120.f);                          // 0.5 s step
    CHECK(t.rebuild(l, 44.f));
    CHECK(closeTo(t.step(), 0.5) && t.count() == 21);
    CHECK(t.text(0) == "0.0" && t.text(1) == "0.5");
    CHECK(t.text(2) == "1.0" && t.text(20) == "10.0");
    CHECK(closeTo(t.x(1), 60.0) && closeTo(t.x(2), 120.0));
    CHECK(!t.rebuild(l, 44.f));                                  // the same: the words are not made again
    // the step grows (a narrower second): fewer marks, words of whole seconds
    l.build(p.data(), p.size(), 20.f);                           // 5 s step... 20 px/s * 5 = 100 >= 44; 2 s * 20 = 40 < 44
    CHECK(t.rebuild(l, 44.f));
    CHECK(closeTo(t.step(), 5.0) && t.count() == 3);
    CHECK(t.text(1) == "5" && t.text(2) == "10");
    // and back to the narrow step with more marks than before the shrink: no old word is left
    l.build(p.data(), p.size(), 120.f);
    CHECK(t.rebuild(l, 44.f));
    CHECK(t.count() == 21 && t.text(1) == "0.5" && t.text(3) == "1.5" && t.text(20) == "10.0");
    // growing again in the same step (a longer timeline): the new marks have words
    std::vector<Piece> longer = piecesOf({ 0, 0 }, { 360 });     // 12 s at the same step
    l.build(longer.data(), longer.size(), 120.f);
    CHECK(t.rebuild(l, 44.f));
    CHECK(t.count() == 25 && t.text(24) == "12.0");
    // shrinking in the same step and growing again: words stay right
    l.build(p.data(), p.size(), 120.f);
    t.rebuild(l, 44.f);
    l.build(longer.data(), longer.size(), 120.f);
    t.rebuild(l, 44.f);
    CHECK(t.count() == 25 && t.text(22) == "11.0" && t.text(24) == "12.0");
    // an empty timeline: one mark at 0
    Layout empty;
    empty.build(nullptr, 0, 60.f);
    t.rebuild(empty, 44.f);
    CHECK(t.count() == 1 && t.text(0).size() > 0);
}

// ---- the drag machine -------------------------------------------------------------------------------------
static void testMachinePlayhead()
{
    DragMachine m;
    CHECK(!m.dragging() && m.state() == DragState::Idle);
    CHECK(m.movePlayhead(1.0).kind == DragOutcome::Kind::None);               // Idle: nothing
    // the ruler: a Seek at once
    DragOutcome o = m.pressPlayhead(0.5, 1.0, true);
    CHECK(m.state() == DragState::Playhead && o.kind == DragOutcome::Kind::Seek && closeTo(o.seek_sec, 1.0));
    CHECK(m.movePlayhead(1.0 + 0.01).kind == DragOutcome::Kind::None);         // the same frame (30 -> 30)
    o = m.movePlayhead(1.1);
    CHECK(o.kind == DragOutcome::Kind::Seek && closeTo(o.seek_sec, 1.1));
    CHECK(closeTo(m.playheadSec(), 1.1));
    // release: a Seek only if the frame is another; then Idle, and a second release says nothing
    o = m.release(1.1);
    CHECK(o.kind == DragOutcome::Kind::None && m.state() == DragState::Idle);
    CHECK(m.release(5.0).kind == DragOutcome::Kind::None);
    CHECK(closeTo(m.playheadSec(), 1.1));                                      // kept after the drag
    // the line: no Seek at the press, even when the mouse is off by a few frames (the caller passes current_sec)
    o = m.pressPlayhead(2.0, 2.0, false);
    CHECK(o.kind == DragOutcome::Kind::None && m.state() == DragState::Playhead);
    // release on another frame: the last Seek is said
    o = m.release(2.5);
    CHECK(o.kind == DragOutcome::Kind::Seek && closeTo(o.seek_sec, 2.5) && !m.dragging());
    // cancel: no event, the position stays where it was last put
    m.pressPlayhead(3.0, 3.0, false);
    m.movePlayhead(3.5);
    CHECK(m.cancel() && !m.dragging() && closeTo(m.playheadSec(), 3.5));
    CHECK(!m.cancel());                                                        // a second cancel: nothing
    CHECK(m.movePlayhead(9.0).kind == DragOutcome::Kind::None);
    // setEditable(false) does not end a playhead drag; onTimeline does not either
    m.pressPlayhead(1.0, 1.0, false);
    CHECK(!m.setEditable(false) && m.state() == DragState::Playhead);
    CHECK(!m.onTimeline(SegmentNow(), 0) && m.state() == DragState::Playhead);
    m.cancel();
}

static void testMachineEdge()
{
    DragMachine m;
    const PairId pair{ 3, 4 };
    // a Smooth segment of 30 frames, a whole of 90, 2 px per frame
    m.pressEdge(2, pair, SegMethod::Smooth, 30, 90, 2.f, 100.f);
    CHECK(m.state() == DragState::Edge && m.segmentIndex() == 2 && m.pair() == pair);
    CHECK(m.edge().frames == 30 && !m.edge().changed);
    CHECK(!m.moveEdge(100.f));                                                 // nothing changed
    CHECK(m.moveEdge(110.f) && m.edge().frames == 35);                        // +5 frames
    CHECK(!m.moveEdge(110.f));                                                 // the same again
    CHECK(m.moveEdge(1000.f) && m.edge().frames == MAX_SMOOTH_INTERVALS);
    CHECK(m.moveEdge(110.f));
    // release: one Resize, then Idle; a second release says nothing
    DragOutcome o = m.release(0.0);
    CHECK(o.kind == DragOutcome::Kind::Resize && o.pair == pair && o.method == SegMethod::Smooth && o.edge.frames == 35);
    CHECK(closeTo(o.edge.seconds, 35.0 / 30.0) && !m.dragging());
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None);
    CHECK(m.segmentIndex() == 2);                                              // kept for the view's clean-up
    // no change: no event
    m.pressEdge(2, pair, SegMethod::Smooth, 30, 90, 2.f, 100.f);
    m.moveEdge(110.f);
    m.moveEdge(100.f);
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None);
    // refused (30 s): no event
    m.pressEdge(0, pair, SegMethod::Smooth, 30, 880, 2.f, 0.f);
    CHECK(m.moveEdge(2000.f) && m.edge().rejected && m.edge().frames == 50);
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None && !m.dragging());
    // a Generate edge: tokens
    m.pressEdge(0, pair, SegMethod::Generate, 33, 100, 2.f, 0.f);
    CHECK(m.moveEdge(8.f) && m.edge().tokens == 11);
    o = m.release(0.0);
    CHECK(o.kind == DragOutcome::Kind::Resize && o.edge.tokens == 11 && o.method == SegMethod::Generate);
    // every way out but the release says nothing: cancel
    m.pressEdge(0, pair, SegMethod::Smooth, 30, 90, 2.f, 0.f);
    m.moveEdge(20.f);
    CHECK(m.cancel() && !m.dragging());
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None);                     // and no Resize afterwards
    CHECK(!m.moveEdge(30.f));                                                  // Idle: moving does nothing
    // setEditable(false)
    m.pressEdge(0, pair, SegMethod::Smooth, 30, 90, 2.f, 0.f);
    m.moveEdge(20.f);
    CHECK(!m.setEditable(true) && m.dragging());
    CHECK(m.setEditable(false) && !m.dragging());
    CHECK(!m.setEditable(false));
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None);
}

static void testMachineTimeline()
{
    DragMachine m;
    const PairId pair{ 1, 2 };
    auto press = [&] { m.pressEdge(1, pair, SegMethod::Smooth, 30, 90, 2.f, 0.f); m.moveEdge(20.f); };
    auto now = [](S32 index, SegMethod method, S32 frames, bool editable) { return SegmentNow{ true, index, method, frames, editable }; };
    // the same segment, as it was (its index may move): the drag goes on
    press();
    CHECK(!m.onTimeline(now(0, SegMethod::Smooth, 30, true), 90) && m.dragging() && m.segmentIndex() == 0);
    // the five ways the segment is not as it was
    CHECK(m.onTimeline(SegmentNow(), 90) && !m.dragging());                                    // gone
    press();
    CHECK(m.onTimeline(now(1, SegMethod::Smooth, 30, false), 90) && !m.dragging());            // queued or running
    press();
    CHECK(m.onTimeline(now(1, SegMethod::Linear, 30, true), 90) && !m.dragging());             // another method
    press();
    CHECK(m.onTimeline(now(1, SegMethod::Smooth, 31, true), 90) && !m.dragging());             // another length
    press();
    CHECK(m.onTimeline(now(1, SegMethod::Smooth, 30, true), 91) && !m.dragging());             // another whole
    // after one of them nothing more is said
    CHECK(m.release(0.0).kind == DragOutcome::Kind::None);
    CHECK(!m.onTimeline(now(1, SegMethod::Smooth, 30, true), 90));
}

static void testRulerHandle()
{
    const std::vector<Piece> p = piecesOf({ 0, 0 }, { 90 });
    Layout l;
    l.build(p.data(), p.size(), 60.f);
    // in the ruler, near the playhead's line (its handle): the playhead, not moved by the press; elsewhere the ruler
    CHECK(hitTest(l, 100.f, 5.f, 100.f).kind == HitKind::Needle);
    CHECK(hitTest(l, 105.f, 5.f, 100.f).kind == HitKind::Needle);
    CHECK(hitTest(l, 106.f, 5.f, 100.f).kind == HitKind::Ruler);
    CHECK(hitTest(l, 94.f, 5.f, 100.f).kind == HitKind::Ruler);
    CHECK(hitTest(l, 95.f, 5.f, 100.f).kind == HitKind::Needle);
}

static void testTickSetShrinkGrow()
{
    // 0.5 s steps (25 marks) -> 5 s steps (3 marks) -> 5 s steps again but more (5 marks): no word of the first is left
    TickSet t;
    std::vector<Piece> a = piecesOf({ 0, 0 }, { 360 });          // 12 s
    Layout l;
    l.build(a.data(), a.size(), 120.f);
    t.rebuild(l, 44.f);
    CHECK(t.count() == 25);
    l.build(a.data(), a.size(), 20.f);                           // 5 s steps: 0, 5, 10
    CHECK(t.rebuild(l, 44.f) && t.count() == 3 && t.text(2) == "10");
    std::vector<Piece> b = piecesOf({ 0, 0 }, { 600 });          // 20 s at 20 px/s
    l.build(b.data(), b.size(), 20.f);
    t.rebuild(l, 44.f);
    CHECK(t.count() == 5 && t.text(3) == "15" && t.text(4) == "20" && t.text(1) == "5");
}

static void testFitHolds()
{
    // two short holds (3 frames = 6 px at 62 px/s, drawn 16) and a 3 s segment in 200 px: the holds are counted at their least
    const std::vector<Piece> p = piecesOf({ 3, 3 }, { 90 });
    const F32 fit = fitPxPerSec(p.data(), p.size(), 200.f, 3.2);
    Layout l;
    l.build(p.data(), p.size(), fit);
    CHECK(closeTo(fit, 56.0));
    CHECK(l.width() <= 200.f + 0.02f && l.width() > 199.f);
}

static void testFocusReturn()
{
    // alive and usable: back to it (directly when it is an ancestor of the strip); else the floater
    CHECK(chooseFocusReturn(true, true, false) == FocusReturn::Previous);
    CHECK(chooseFocusReturn(true, true, true) == FocusReturn::PreviousDirect);
    CHECK(chooseFocusReturn(false, true, false) == FocusReturn::Floater);   // gone
    CHECK(chooseFocusReturn(true, false, false) == FocusReturn::Floater);   // hidden or disabled meanwhile
    CHECK(chooseFocusReturn(true, false, true) == FocusReturn::Floater);
    CHECK(chooseFocusReturn(false, false, true) == FocusReturn::Floater);
}

static void testScrollShare()
{
    CHECK(closeTo(scrollShare(0.f, 900.f, 500.f), 0.0));
    CHECK(closeTo(scrollShare(200.f, 900.f, 500.f), 0.5));
    CHECK(closeTo(scrollShare(400.f, 900.f, 500.f), 1.0));
    CHECK(closeTo(scrollShare(700.f, 600.f, 500.f), 1.0));      // past the end (the strip shrank under an edge drag)
    CHECK(closeTo(scrollShare(50.f, 400.f, 500.f), 0.0));       // fits: no range
    CHECK(closeTo(scrollShare(std::numeric_limits<F32>::quiet_NaN(), 900.f, 500.f), 0.0));
    CHECK(closeTo(scrollShare(-10.f, 900.f, 500.f), 0.0));
}

int main()
{
    testPxPerSec();
    testScroll();
    testTicks();
    testTicks44();
    testHoldMinAndExact();
    testFit();
    testTickSet();
    testMachinePlayhead();
    testMachineEdge();
    testMachineTimeline();
    testFocusReturn();
    testScrollShare();
    testRulerHandle();
    testTickSetShrinkGrow();
    testFitHolds();
    testLayoutLinear();
    testLayoutHoldAndMinWidth();
    testLayoutEmpty();
    testHit();
    testHoldHit();
    testDragGenerate();
    testDragSmooth();
    testDragLimit();
    testDragBadInput();
    if (failures == 0)
    {
        std::printf("all timeline view tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
