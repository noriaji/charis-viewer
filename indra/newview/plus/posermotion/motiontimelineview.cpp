/**
 * @file motiontimelineview.cpp
 * @brief The timeline strip of the motion window (see motiontimelineview.h).
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

#include "llviewerprecompiledheaders.h"

#include "motiontimelineview.h"

#include "plus/photoscreen/psui/psstyle.h"

#include "llfloater.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "lltooltip.h"
#include "lluictrlfactory.h"
#include "llwindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plus::posermotion
{
    using namespace plus::psui;
    using namespace plus::posermotion::tlgeom;

    static_assert((size_t)JobError::PrevMissing + 1 == MotionTimelineView::Strings::JOB_ERRORS,
                  "Strings::job_error has one word for each JobError");

    namespace
    {
        LLDefaultChildRegistry::Register<MotionTimelineView> sTimelineViewRegistration("plus_motion_timeline");

        constexpr F32 SEGMENT_GAP      = 1.f;     // each side of a segment (the keys' diamonds sit on the joints)
        constexpr F32 TICK_MIN_GAP_PX  = 44.f;    // marks of the ruler at least this far apart
        constexpr F32 FOLLOW_MARGIN_PX = 24.f;    // the playhead kept this far from the strip's ends
        constexpr F32 WHEEL_STEP_PX    = 48.f;
        constexpr F64 FOLLOW_PAUSE_SEC = 2.0;     // after the user scrolled, the strip does not follow the playhead
        constexpr F32 MADE_FILL        = 0.9f;    // the fill of a made segment (S3)
        constexpr F32 FAIL_FILL        = 0.3f;    // of a failed one (S5)
        constexpr F32 RUNNING_FILL     = 0.25f;   // of a running one (S2): this, plus RUNNING_SWING at the top of its breath
        constexpr F32 RUNNING_SWING    = 0.4f;
        constexpr F32 OUTLINE_FILL     = 0.16f;   // the fill of an outlined segment (S0, S1)
        constexpr F32 OUTLINE_RING     = 0.9f;    // and its ring
        constexpr S32 FAIL_MARK_PX     = 14;      // room of the "!" of a failed segment, taken off its label's room
        constexpr S32 REGEN_MARK_PX    = 10;      // room of the dot of "regenerating is advised"

        /// The colour of a method: Generate is the accent, Smooth a bluish grey, Linear a grey.
        const LLColor4& methodColor(SegMethod method)
        {
            switch (method)
            {
            case SegMethod::Smooth: return style::TL_SMOOTH;
            case SegMethod::Linear: return style::TL_LINEAR;
            case SegMethod::Generate: break;
            }
            return style::ACCENT;
        }

        void fillDiamond(F32 cx, F32 cy, F32 r, const LLColor4& color)
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.color4fv(color.mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(cx - r, cy);
            gGL.vertex2f(cx, cy - r);
            gGL.vertex2f(cx + r, cy);
            gGL.vertex2f(cx - r, cy);
            gGL.vertex2f(cx + r, cy);
            gGL.vertex2f(cx, cy + r);
            gGL.end();
        }

        S32 widthOf(const LLFontGL* font, const LLWString& text)
        {
            return font ? (S32)font->getWidth(text.c_str()) : 0;
        }
    } // namespace

    // =============================================================================================
    // set-up

    MotionTimelineView::Params::Params()
    {
        mouse_opaque(true);
        tab_stop(false);
    }

    MotionTimelineView::MotionTimelineView(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_SMALL)),
        mWarn(utf8str_to_wstring("!"))
    {
        mKeys.reserve(TIMELINE_MAX_KEYS);
        mSegs.reserve(TIMELINE_MAX_KEYS);
        mPieces.reserve(3 * TIMELINE_MAX_KEYS);
        rebuildLayout(true);
    }

    MotionTimelineView::~MotionTimelineView()
    {
        mMachine.cancel();
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
    }

    // =============================================================================================
    // what the window gives

    bool MotionTimelineView::sameAsView(const MotionTimeline& timeline) const
    {
        if ((size_t)timeline.keyCount() != mKeys.size() || (size_t)timeline.segmentCount() != mSegs.size()
            || std::max(0, timeline.totalFrames() - 1) != mTotalIntervals)
        {
            return false;
        }
        for (S32 i = 0; i < timeline.keyCount(); ++i)
        {
            const KeyView& k = mKeys[(size_t)i];
            if (k.id != timeline.key(i).id || k.hold_frames != timeline.holdFrames(i) || k.hold_sec != timeline.key(i).hold_sec)
            {
                return false;
            }
        }
        for (S32 i = 0; i < timeline.segmentCount(); ++i)
        {
            const SegmentInfo& s = timeline.segment(i);
            const SegView&     v = mSegs[(size_t)i];
            if (v.pair != s.pair || v.method != s.method || v.state != s.state || v.fail != s.fail || v.frames != s.frames
                || v.regen != s.regen_reasons)
            {
                return false;
            }
        }
        return true;
    }

    void MotionTimelineView::setTimeline(const MotionTimeline& timeline)
    {
        if (sameAsView(timeline))
        {
            return;
        }
        mKeys.clear();
        for (S32 i = 0; i < timeline.keyCount(); ++i)
        {
            mKeys.push_back(KeyView{ timeline.key(i).id, timeline.holdFrames(i), timeline.key(i).hold_sec });
        }
        mSegs.clear();
        for (S32 i = 0; i < timeline.segmentCount(); ++i)
        {
            const SegmentInfo& s = timeline.segment(i);
            SegView            v;
            v.pair   = s.pair;
            v.method = s.method;
            v.state  = s.state;
            v.fail   = s.fail;
            v.frames = s.frames;
            v.regen  = s.regen_reasons;
            setLabels(v, v.frames);
            mSegs.push_back(std::move(v));
        }
        mTotalIntervals = std::max(0, timeline.totalFrames() - 1);
        mHoverHit       = {};
        mTipValid       = false;

        if (mMachine.state() == DragState::Edge)
        {
            // the segment that is dragged must still be there, as it was
            S32 found = -1;
            for (size_t i = 0; i < mSegs.size(); ++i)
            {
                if (mSegs[i].pair == mMachine.pair())
                {
                    found = (S32)i;
                    break;
                }
            }
            SegmentNow now;
            if (found >= 0)
            {
                now = { true, found, mSegs[(size_t)found].method, mSegs[(size_t)found].frames, segmentEditable(found) };
            }
            if (mMachine.onTimeline(now, mTotalIntervals))
            {
                finishDrag(DragState::Edge, false);     // the layout is made below
            }
            else
            {
                setLabels(mSegs[(size_t)found], mMachine.edge().frames);   // the label of the length being dragged
            }
        }
        rebuildLayout(mMachine.state() != DragState::Edge);
    }

    void MotionTimelineView::setSelection(KeyId key, PairId pair)
    {
        mSelectedKey  = key;
        mSelectedPair = pair;
    }

    void MotionTimelineView::setPlayheadSec(F64 seconds)
    {
        const F64 sec = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        if (sec == mPlayheadSec)
        {
            return;
        }
        mPlayheadSec = sec;
        showPlayhead();
    }

    void MotionTimelineView::setStrings(const Strings& strings)
    {
        mStrings = strings;
        for (SegView& s : mSegs)
        {
            setLabels(s, s.frames);
        }
        if (mMachine.state() == DragState::Edge && mMachine.segmentIndex() >= 0 && (size_t)mMachine.segmentIndex() < mSegs.size())
        {
            setLabels(mSegs[(size_t)mMachine.segmentIndex()], mMachine.edge().frames);
        }
        mTipValid = false;
        rebuildLayout(mMachine.state() != DragState::Edge);
    }

    void MotionTimelineView::setEditable(bool editable)
    {
        mEditable = editable;
        const DragState was = mMachine.state();
        if (mMachine.setEditable(editable))
        {
            finishDrag(was, true);
        }
    }

    LLSD MotionTimelineView::getValue() const
    {
        LLSD v;
        switch (mEvent.kind)
        {
        case Event::Kind::Seek:
            v["kind"]     = "seek";
            v["seek_sec"] = mEvent.seek_sec;
            break;
        case Event::Kind::Resize:
            v["kind"]    = "resize";
            v["from"]    = (LLSD::Integer)mEvent.pair.from;
            v["to"]      = (LLSD::Integer)mEvent.pair.to;
            v["method"]  = (LLSD::Integer)mEvent.method;
            v["tokens"]  = mEvent.tokens;
            v["seconds"] = mEvent.seconds;
            break;
        case Event::Kind::None:
            break;
        }
        return v;
    }

    // =============================================================================================
    // labels and layout (when the timeline or the width changes; never while drawing)

    const std::string& MotionTimelineView::methodName(SegMethod method) const
    {
        switch (method)
        {
        case SegMethod::Smooth: return mStrings.smooth;
        case SegMethod::Linear: return mStrings.linear;
        case SegMethod::Generate: break;
        }
        return mStrings.generate;
    }

    const std::string& MotionTimelineView::stateName(SegState state) const
    {
        switch (state)
        {
        case SegState::Queued:  return mStrings.queued;
        case SegState::Running: return mStrings.running;
        case SegState::Made:    return mStrings.made;
        case SegState::Failed:  return mStrings.failed;
        case SegState::Unmade:  break;
        }
        return mStrings.unmade;
    }

    std::string MotionTimelineView::regenText(U8 reasons) const
    {
        static_assert(REGEN_ENDS == 1 && REGEN_TARGETS == 2 && REGEN_CONTEXT == 4 && REGEN_PASS == 8 && Strings::REGEN_REASONS == 4,
                      "Strings::regen_reason is in the order of the REGEN_* bits");
        std::string text  = mStrings.regen;
        bool        first = true;
        for (size_t i = 0; i < Strings::REGEN_REASONS; ++i)
        {
            if ((reasons & (1u << i)) == 0 || mStrings.regen_reason[i].empty())
            {
                continue;
            }
            text += first ? mStrings.reason_prefix : mStrings.regen_join;
            text += mStrings.regen_reason[i];
            first = false;
        }
        return text;
    }

    std::string MotionTimelineView::reasonOf(JobError error) const
    {
        const size_t i = (size_t)error;
        return i < mStrings.job_error.size() && !mStrings.job_error[i].empty() ? mStrings.job_error[i] : std::string(describe(error));
    }

    /// The words on a segment of `frames` steps (the length shown: the model's, or the one being dragged).
    void MotionTimelineView::setLabels(SegView& seg, S32 frames) const
    {
        char secs[24];
        std::snprintf(secs, sizeof(secs), " %.1f", (F64)frames / (F64)CLIP_FPS);
        const std::string& name = methodName(seg.method);
        seg.label_short = utf8str_to_wstring(name);
        seg.label_full  = utf8str_to_wstring(name + secs + mStrings.second);
        seg.short_width = widthOf(mFont, seg.label_short);
        seg.full_width  = widthOf(mFont, seg.label_full);
    }

    /// The layout from the copy of the timeline (the dragged segment at the length being dragged, drawn at its true
    /// width). new_scale: the width of a second is worked out again from the strip's width and the whole's length
    /// (the minimum widths of short segments counted); false: it stays (a drag).
    void MotionTimelineView::rebuildLayout(bool new_scale)
    {
        const bool edge = mMachine.state() == DragState::Edge;
        mPieces.clear();
        S32    whole = 0;
        size_t j     = 0;
        for (size_t i = 0; i < mKeys.size(); ++i)
        {
            mPieces.push_back({ SpanKind::Key, (S32)i, 0, false });
            if (mKeys[i].hold_frames > 0)
            {
                mPieces.push_back({ SpanKind::Hold, (S32)i, mKeys[i].hold_frames, false });
                whole += mKeys[i].hold_frames;
            }
            if (i + 1 < mKeys.size() && j < mSegs.size() && mSegs[j].pair.from == mKeys[i].id)
            {
                const bool dragged = edge && (S32)j == mMachine.segmentIndex();
                const S32  frames  = dragged ? mMachine.edge().frames : mSegs[j].frames;
                mPieces.push_back({ SpanKind::Segment, (S32)j, frames, dragged });
                whole += frames;
                ++j;
            }
        }
        const F32 usable = (F32)getRect().getWidth() - 2.f * MARGIN_PX;
        const F32 scale  = new_scale ? fitPxPerSec(mPieces.data(), mPieces.size(), usable, (F64)whole / (F64)CLIP_FPS)
                                     : mLayout.pxPerSec();
        mLayout.build(mPieces.data(), mPieces.size(), scale);

        for (const Span& span : mLayout.spans())
        {
            if (span.kind == SpanKind::Segment && (size_t)span.index < mSegs.size())
            {
                SegView&  seg     = mSegs[(size_t)span.index];
                const S32 reserve = (seg.state == SegState::Failed ? FAIL_MARK_PX : 0) + (seg.regen != 0 ? REGEN_MARK_PX : 0);
                const S32 avail   = (S32)(span.x1 - span.x0 - 2.f * SEGMENT_GAP - 2.f * style::TL_LABEL_PAD) - reserve;
                seg.label_mode    = seg.full_width <= avail ? 2 : (seg.short_width <= avail ? 1 : 0);
            }
        }
        rebuildTicks();
        if (!edge)
        {
            setScroll(mScroll);     // not while an edge is dragged: a shrinking strip would carry the edge away from the mouse
        }
        mTipValid = false;
    }

    void MotionTimelineView::rebuildTicks()
    {
        if (mTicks.rebuild(mLayout, TICK_MIN_GAP_PX))
        {
            mTickText.resize((size_t)mTicks.count());
            mTickWidth.resize((size_t)mTicks.count());
            for (S32 i = 0; i < mTicks.count(); ++i)
            {
                mTickText[(size_t)i]  = utf8str_to_wstring(mTicks.text(i));
                mTickWidth[(size_t)i] = widthOf(mFont, mTickText[(size_t)i]);
            }
        }
    }

    void MotionTimelineView::reshape(S32 width, S32 height, bool called_from_parent)
    {
        PsCtrl::reshape(width, height, called_from_parent);
        rebuildLayout(mMachine.state() != DragState::Edge);
    }

    void MotionTimelineView::setScroll(F32 scroll)
    {
        mScroll = clampScroll(scroll, mLayout.contentWidth(), (F32)getRect().getWidth());
    }

    F64 MotionTimelineView::playheadFrame() const
    {
        return (mMachine.state() == DragState::Playhead ? mMachine.playheadSec() : mPlayheadSec) * (F64)CLIP_FPS;
    }

    F64 MotionTimelineView::seekSecAt(S32 view_x) const
    {
        return seekSecOfX(mLayout, contentX(view_x));
    }

    void MotionTimelineView::showPlayhead()
    {
        if (mMachine.dragging() || (F64)mPulse.getElapsedTimeF32() < mFollowAfter)
        {
            return;
        }
        const F32 page_x = mLayout.xOfFrame(playheadFrame()) + MARGIN_PX;
        mScroll = scrollToShow(mScroll, page_x, mLayout.contentWidth(), (F32)getRect().getWidth(), FOLLOW_MARGIN_PX);
    }

    // =============================================================================================
    // drawing: rectangles and the words made before. Nothing is allocated here.

    void MotionTimelineView::draw()
    {
        const LLRectf r = localRectf();
        const F32     a = drawAlpha();
        LLLocalClipRect clip(getLocalRect());
        drawRoundBordered(r, style::TL_RADIUS, Fill::solid(faded(style::TL_FILL, a)), faded(style::TL_BORDER, a));
        drawRuler(a);
        for (const Span& span : mLayout.spans())
        {
            if (span.kind == SpanKind::Segment && (size_t)span.index < mSegs.size())
            {
                drawSegment(mSegs[(size_t)span.index], span.index, span, a);
            }
            else if (span.kind == SpanKind::Hold)
            {
                drawHold(span, a);
            }
        }
        for (const Span& span : mLayout.spans())
        {
            if (span.kind == SpanKind::Key)
            {
                drawKey(span, a);
            }
        }
        drawNeedle(a);
        drawScrollIndicator(a);
        if (hasFocus())
        {
            drawFocusRing(r, style::TL_RADIUS);
        }
    }

    void MotionTimelineView::drawRuler(F32 a)
    {
        const F32 H     = (F32)getRect().getHeight();
        const F32 width = (F32)getRect().getWidth();
        fillRect(LLRectf(MARGIN_PX * 0.5f, H - RULER_H, width - MARGIN_PX * 0.5f, H - RULER_H - 1.f),
                 Fill::solid(faded(style::TL_RULER, a)));
        const LLColor4 tick = faded(style::TL_TICK, a);
        const LLColor4 text = faded(style::TEXT_DIM, a);
        for (S32 i = 0; i < mTicks.count() && (size_t)i < mTickText.size(); ++i)
        {
            const F32 x = viewX(mTicks.x(i));
            if (x < -mTickWidth[(size_t)i] || x > width)
            {
                continue;
            }
            fillRect(LLRectf(x, H - RULER_H + 5.f, x + 1.f, H - RULER_H), Fill::solid(tick));
            drawText(mFont, mTickText[(size_t)i], x + 3.f, H - RULER_H * 0.5f - 1.f, text, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
    }

    void MotionTimelineView::drawSegment(const SegView& seg, S32 index, const Span& span, F32 a)
    {
        const F32 H     = (F32)getRect().getHeight();
        const F32 width = (F32)getRect().getWidth();
        const F32 x0    = viewX(span.x0) + SEGMENT_GAP;
        const F32 x1    = viewX(span.x1) - SEGMENT_GAP;
        if (x1 < 0.f || x0 > width)
        {
            return;
        }
        const LLRectf rc(x0, H - TRACK_TOP, x1, H - TRACK_TOP - TRACK_H);
        const bool    failed = seg.state == SegState::Failed;
        const LLColor4& base = failed ? style::TL_FAIL : methodColor(seg.method);
        const F32     radius = style::TL_SEG_RADIUS;
        switch (seg.state)
        {
        case SegState::Made:
            drawRound(rc, radius, Fill::solid(faded(base, MADE_FILL * a)));
            break;
        case SegState::Failed:
            drawRound(rc, radius, Fill::solid(faded(base, FAIL_FILL * a)));
            drawRing(rc, radius, 1.f, faded(base, a));
            break;
        case SegState::Running:
        {
            const F32 pulse = 0.5f + 0.5f * std::sin(mPulse.getElapsedTimeF32() * 5.f);
            drawRound(rc, radius, Fill::solid(faded(base, (RUNNING_FILL + RUNNING_SWING * pulse) * a)));
            drawRing(rc, radius, 1.f, faded(base, a));
            break;
        }
        case SegState::Unmade:
        case SegState::Queued:
            // a stand-in: outlined, not filled (S0 plays as a smooth motion until it is generated; S1 waits)
            drawRound(rc, radius, Fill::solid(faded(base, OUTLINE_FILL * a)));
            drawRing(rc, radius, 1.f, faded(base, OUTLINE_RING * a));
            break;
        }
        if (seg.pair == mSelectedPair && mSelectedPair.valid())
        {
            drawRing(LLRectf(rc.mLeft - 1.f, rc.mTop + 1.f, rc.mRight + 1.f, rc.mBottom - 1.f), radius + 1.f, 1.5f,
                     faded(style::ACCENT_LINE, a));
        }
        const F32 mid = (rc.mTop + rc.mBottom) * 0.5f;
        if (seg.label_mode > 0)
        {
            drawText(mFont, seg.label_mode == 2 ? seg.label_full : seg.label_short, x0 + style::TL_LABEL_PAD, mid,
                     faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        if (failed)
        {
            drawText(mFont, mWarn, x1 - 5.f, mid, faded(style::TEXT_WHITE, a), LLFontGL::RIGHT, LLFontGL::VCENTER);
        }
        if (seg.regen != 0)
        {
            drawCircle(x1 - 6.f, rc.mTop - 6.f, style::TL_DOT_RADIUS, Fill::solid(faded(style::ACCENT_LINE, a)));
        }
        // the right edge: lit when it can be grabbed and the mouse is on it, red when the drag is refused
        const bool dragged = mMachine.state() == DragState::Edge && mMachine.segmentIndex() == index;
        const bool hovered = mHoverHit.kind == HitKind::SegmentEdge && mHoverHit.index == index && !mMachine.dragging()
                             && getEnabled() && segmentEditable(index);
        if (dragged || hovered)
        {
            const LLColor4& c = dragged && mMachine.edge().rejected ? style::TL_FAIL : style::TEXT_WHITE;
            fillRect(LLRectf(rc.mRight - 1.5f, rc.mTop - 3.f, rc.mRight + 1.5f, rc.mBottom + 3.f), Fill::solid(faded(c, a)));
        }
    }

    void MotionTimelineView::drawHold(const Span& span, F32 a)
    {
        const F32 H   = (F32)getRect().getHeight();
        const F32 mid = H - TRACK_TOP - TRACK_H * 0.5f;
        const F32 x0  = viewX(span.x0);
        const F32 x1  = viewX(span.x1);
        if (x1 < 0.f || x0 > (F32)getRect().getWidth())
        {
            return;
        }
        drawRound(LLRectf(x0, mid + HOLD_H * 0.5f, x1, mid - HOLD_H * 0.5f), style::TL_HOLD_RADIUS,
                  Fill::solid(faded(style::TL_HOLD, a)));
    }

    void MotionTimelineView::drawKey(const Span& span, F32 a)
    {
        const F32 H   = (F32)getRect().getHeight();
        const F32 cx  = viewX(span.x0);
        if (cx < -DIAMOND_R - 2.f || cx > (F32)getRect().getWidth() + DIAMOND_R + 2.f)
        {
            return;
        }
        const F32  cy  = H - TRACK_TOP - TRACK_H * 0.5f;
        const bool sel = (size_t)span.index < mKeys.size() && mKeys[(size_t)span.index].id == mSelectedKey
                         && mSelectedKey != NO_KEY;
        fillDiamond(cx, cy, DIAMOND_R + 1.5f, faded(style::TL_FILL, a));
        fillDiamond(cx, cy, DIAMOND_R, faded(sel ? style::ACCENT_LINE : style::TEXT_WHITE, a));
    }

    void MotionTimelineView::drawNeedle(F32 a)
    {
        const F32 H = (F32)getRect().getHeight();
        const F32 x = viewX(mLayout.xOfFrame(playheadFrame()));
        if (x < -6.f || x > (F32)getRect().getWidth() + 6.f)
        {
            return;
        }
        fillRect(LLRectf(x - 1.f, H - RULER_H * 0.5f, x + 1.f, H - TRACK_TOP - TRACK_H - 4.f), Fill::solid(faded(style::TEXT_WHITE, a)));
        const LLColor4& handle = mMachine.state() == DragState::Playhead ? style::ACCENT_LINE : style::ACCENT;
        drawRound(LLRectf(x - 5.f, H - 1.f, x + 5.f, H - RULER_H + 3.f), style::TL_HANDLE_RADIUS, Fill::solid(faded(handle, a)));
    }

    void MotionTimelineView::drawScrollIndicator(F32 a)
    {
        const F32 width   = (F32)getRect().getWidth();
        const F32 content = mLayout.contentWidth();
        if (content <= width + 0.5f)
        {
            return;
        }
        const F32 H     = (F32)getRect().getHeight();
        const F32 thumb = std::max(16.f, width * width / content);
        const F32 x     = (width - thumb) * scrollShare(mScroll, content, width);
        drawRound(LLRectf(x, H - SCROLLBAR_TOP, x + thumb, H - SCROLLBAR_TOP - SCROLLBAR_H), SCROLLBAR_H * 0.5f,
                  Fill::solid(faded(style::TL_SCROLL, a)));
    }

    // =============================================================================================
    // the mouse

    /// What is under the mouse at view (x, y), y up from the bottom as the view's mouse functions have it.
    Hit MotionTimelineView::hitAt(S32 x, S32 y) const
    {
        return hitTest(mLayout, contentX(x), (F32)getRect().getHeight() - (F32)y, mLayout.xOfFrame(playheadFrame()));
    }

    bool MotionTimelineView::segmentEditable(S32 index) const
    {
        if (!mEditable || index < 0 || (size_t)index >= mSegs.size())
        {
            return false;
        }
        const SegState s = mSegs[(size_t)index].state;
        return s != SegState::Queued && s != SegState::Running;   // the model refuses (Busy) for those
    }

    bool MotionTimelineView::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        if (!getEnabled())
        {
            return true;
        }
        endDrag();
        const Hit hit = hitAt(x, y);
        if ((hit.kind == HitKind::Ruler || hit.kind == HitKind::Needle) && !mSegs.empty())
        {
            // nothing to seek in without a clip (fewer than two keys: a hold alone makes a layout but no clip).
            // A press on the line or its handle does not move the playhead.
            const bool from_ruler = hit.kind == HitKind::Ruler;
            const DragOutcome out = mMachine.pressPlayhead(mPlayheadSec, from_ruler ? seekSecAt(x) : mPlayheadSec, from_ruler);
            gFocusMgr.setMouseCapture(this);
            takeFocusForDrag();
            fire(out);
        }
        else if (hit.kind == HitKind::SegmentEdge && segmentEditable(hit.index))
        {
            const SegView& seg = mSegs[(size_t)hit.index];
            mMachine.pressEdge(hit.index, seg.pair, seg.method, seg.frames, mTotalIntervals, mLayout.pxPerFrame(), (F32)x);
            gFocusMgr.setMouseCapture(this);
            takeFocusForDrag();
            rebuildLayout(false);       // the dragged segment is drawn at its true width from now on
        }
        return true;
    }

    /// The second press of a quick pair arrives as a double click and the first handler would not see it (as LLButton).
    bool MotionTimelineView::handleDoubleClick(S32 x, S32 y, MASK mask)
    {
        return handleMouseDown(x, y, mask);
    }

    /// While a drag goes on a right press is taken here: it would go on to the world's pie menu otherwise.
    bool MotionTimelineView::handleRightMouseDown(S32 x, S32 y, MASK mask)
    {
        return mMachine.dragging() ? true : PsCtrl::handleRightMouseDown(x, y, mask);
    }

    bool MotionTimelineView::handleMiddleMouseDown(S32 x, S32 y, MASK mask)
    {
        return mMachine.dragging() ? true : PsCtrl::handleMiddleMouseDown(x, y, mask);
    }

    bool MotionTimelineView::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (!hasMouseCapture() || !mMachine.dragging())
        {
            return true;
        }
        // the strip goes back to Idle first: the event is the last thing, and the window answers with a new timeline
        const DragState was = mMachine.state();
        DragOutcome     out;
        if (getEnabled())
        {
            out = mMachine.release(seekSecAt(x));
        }
        else
        {
            mMachine.cancel();
        }
        finishDrag(was, true);
        fire(out);
        return true;
    }

    bool MotionTimelineView::handleHover(S32 x, S32 y, MASK mask)
    {
        if (mMachine.dragging())
        {
            if (!getEnabled() || !hasMouseCapture())
            {
                endDrag();
                return true;
            }
            if (mMachine.state() == DragState::Playhead)
            {
                fire(mMachine.movePlayhead(seekSecAt(x)));
            }
            else
            {
                getWindow()->setCursor(UI_CURSOR_SIZEWE);
                if (mMachine.moveEdge((F32)x))
                {
                    const S32 index = mMachine.segmentIndex();
                    if (index >= 0 && (size_t)index < mSegs.size())
                    {
                        setLabels(mSegs[(size_t)index], mMachine.edge().frames);
                    }
                    rebuildLayout(false);
                }
            }
            return true;
        }
        mHoverHit = hitAt(x, y);
        if (mHoverHit.kind == HitKind::SegmentEdge && segmentEditable(mHoverHit.index) && getEnabled())
        {
            getWindow()->setCursor(UI_CURSOR_SIZEWE);
            return true;
        }
        return PsCtrl::handleHover(x, y, mask);
    }

    void MotionTimelineView::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHoverHit = {};
        PsCtrl::onMouseLeave(x, y, mask);
    }

    bool MotionTimelineView::handleKeyHere(KEY key, MASK mask)
    {
        if (mMachine.dragging() && key == KEY_ESCAPE && mask == MASK_NONE)
        {
            endDrag();    // given up: nothing is sent
            return true;
        }
        // not PsCtrl's: Space and Return would be taken for a click that this view does not have
        return LLUICtrl::handleKeyHere(key, mask);
    }

    bool MotionTimelineView::handleScrollWheel(S32 x, S32 y, S32 clicks)
    {
        const F32 width = (F32)getRect().getWidth();
        if (mLayout.contentWidth() <= width + 0.5f)
        {
            return false;      // nothing to scroll here: whoever is behind may scroll
        }
        if (mMachine.state() == DragState::Edge)
        {
            return true;       // the edge would run away from the mouse (it is placed by how far the mouse moved)
        }
        setScroll(mScroll + (F32)clicks * WHEEL_STEP_PX);
        mFollowAfter = (F64)mPulse.getElapsedTimeF32() + FOLLOW_PAUSE_SEC;
        return true;
    }

    /// The words of the tool tip of what is under the mouse (made when it changes, not at every call).
    void MotionTimelineView::makeTip(const Hit& hit)
    {
        char secs[24];
        mTipText.clear();
        if (hit.kind == HitKind::Hold)
        {
            if ((size_t)hit.index < mKeys.size())
            {
                std::snprintf(secs, sizeof(secs), " %.1f", (F64)mKeys[(size_t)hit.index].hold_sec);
                mTipText = mStrings.hold + secs + mStrings.second;
            }
        }
        else if ((size_t)hit.index < mSegs.size())
        {
            const SegView& seg = mSegs[(size_t)hit.index];
            std::snprintf(secs, sizeof(secs), " %.1f", (F64)seg.frames / (F64)CLIP_FPS);
            mTipText = methodName(seg.method) + secs + mStrings.second + mStrings.separator + stateName(seg.state);
            // a failure's reason; PrevMissing is said on S0 / S3 too (the segment went back without a word of its own)
            if (seg.fail != JobError::None && (seg.state == SegState::Failed || seg.fail == JobError::PrevMissing))
            {
                mTipText += mStrings.reason_prefix + reasonOf(seg.fail);
            }
            if (seg.regen != 0)
            {
                mTipText += mStrings.note_prefix + regenText(seg.regen);
            }
        }
        mTipHit   = hit;
        mTipValid = true;
    }

    bool MotionTimelineView::handleToolTip(S32 x, S32 y, MASK mask)
    {
        if (mMachine.dragging())
        {
            return true;
        }
        const Hit hit = hitAt(x, y);
        const bool segment = hit.kind == HitKind::Segment || hit.kind == HitKind::SegmentEdge;
        if (!segment && hit.kind != HitKind::Hold)
        {
            return PsCtrl::handleToolTip(x, y, mask);
        }
        if (!mTipValid || !(mTipHit == hit))
        {
            makeTip(hit);
        }
        const Span* span = mLayout.find(segment ? SpanKind::Segment : SpanKind::Hold, hit.index);
        if (!span || mTipText.empty())
        {
            return PsCtrl::handleToolTip(x, y, mask);
        }
        const S32 H = getRect().getHeight();
        LLRect    screen;
        localRectToScreen(LLRect((S32)viewX(span->x0), H - (S32)TRACK_TOP, (S32)viewX(span->x1), H - (S32)(TRACK_TOP + TRACK_H)), &screen);
        LLToolTipMgr::instance().show(LLToolTip::Params().message(mTipText).sticky_rect(screen));
        return true;
    }

    // =============================================================================================
    // the drags: the machine decides; these do what it says

    /// An event for the window: the last thing of a handler (the window may answer with anything).
    void MotionTimelineView::fire(const DragOutcome& outcome)
    {
        switch (outcome.kind)
        {
        case DragOutcome::Kind::Seek:
            mEvent          = Event();
            mEvent.kind     = Event::Kind::Seek;
            mEvent.seek_sec = outcome.seek_sec;
            break;
        case DragOutcome::Kind::Resize:
            mEvent         = Event();
            mEvent.kind    = Event::Kind::Resize;
            mEvent.pair    = outcome.pair;
            mEvent.method  = outcome.method;
            mEvent.tokens  = outcome.edge.tokens;
            mEvent.seconds = outcome.edge.seconds;
            break;
        case DragOutcome::Kind::None:
            return;
        }
        onCommit();
    }

    void MotionTimelineView::takeFocusForDrag()
    {
        // Esc needs the keyboard focus. The holder before (a field, the floater) is remembered and gets it back at the end:
        // LLUICtrl::setFocus(false) would leave the focus with nobody (the floater then turns inactive, keys go to the world).
        mTookFocus = !hasFocus();
        mPrevFocus.markDead();
        if (mTookFocus)
        {
            if (LLUICtrl* previous = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus()))
            {
                mPrevFocus = previous->getHandle();
            }
        }
        setFocus(true);
    }

    /// Everything that is not a release ends here: Esc, focus or capture lost, hidden, disabled. No event.
    void MotionTimelineView::endDrag()
    {
        const DragState was = mMachine.state();
        if (mMachine.cancel())
        {
            finishDrag(was, true);
        }
    }

    /// After the machine is Idle (whatever way it got there): the capture and the focus let go, the display the
    /// model's. relayout false: the caller makes the layout itself (setTimeline).
    void MotionTimelineView::finishDrag(DragState was, bool relayout)
    {
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);     // calls onMouseCaptureLost: the machine is Idle, so it does nothing
        }
        if (mTookFocus)
        {
            mTookFocus = false;
            LLUICtrl* previous = mPrevFocus.get();
            mPrevFocus.markDead();
            if (hasFocus())
            {
                const bool usable = previous && previous != this && previous->isInVisibleChain() && previous->isInEnabledChain();
                switch (chooseFocusReturn(usable, usable, usable && hasAncestor(previous)))
                {
                case FocusReturn::Previous:
                    previous->setFocus(true);
                    break;
                case FocusReturn::PreviousDirect:
                    gFocusMgr.setKeyboardFocus(previous);
                    break;
                case FocusReturn::Floater:
                    if (LLFloater* floater = getParentByType<LLFloater>())
                    {
                        gFocusMgr.setKeyboardFocus(floater);
                    }
                    else
                    {
                        setFocus(false);
                    }
                    break;
                }
            }
        }
        if (was == DragState::Playhead)
        {
            mPlayheadSec = mMachine.playheadSec();  // it stays where the drag put it (the window was told each frame)
        }
        else if (was == DragState::Edge)
        {
            const S32 index = mMachine.segmentIndex();
            if (index >= 0 && (size_t)index < mSegs.size())
            {
                setLabels(mSegs[(size_t)index], mSegs[(size_t)index].frames);
            }
            if (relayout)
            {
                rebuildLayout(true);
            }
        }
    }

    void MotionTimelineView::setEnabled(bool enabled)
    {
        if (!enabled)
        {
            endDrag();
        }
        PsCtrl::setEnabled(enabled);
    }

    void MotionTimelineView::onMouseCaptureLost()
    {
        PsCtrl::onMouseCaptureLost();
        endDrag();
    }

    void MotionTimelineView::onFocusLost()
    {
        mTookFocus = false;     // it is gone already
        mPrevFocus.markDead();
        endDrag();
        PsCtrl::onFocusLost();
    }

    void MotionTimelineView::onVisibilityChange(bool new_visibility)
    {
        if (!new_visibility)
        {
            endDrag();
            mHoverHit = {};
        }
        PsCtrl::onVisibilityChange(new_visibility);
    }
} // namespace plus::posermotion
