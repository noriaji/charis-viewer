/**
 * @file motiontimelineview.h
 * @brief The timeline strip of the motion window (research/357 section 2): a view that shows the keys, the
 *        segments (by method and state), the holds, the ruler in seconds and the playhead, and lets the user drag the
 *        playhead and the right edge of a segment. XUI tag: plus_motion_timeline.
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

// What the strip does (and nothing else; the keys and segments are chosen, moved, copied, deleted and set on the
// chips and the fields of the window):
//   - shows the timeline, which the window hands over with setTimeline() (a copy: the strip never points into the
//     model, and never writes to it);
//   - drags the playhead: a press on the ruler or on the playhead's line; each change of the frame fires an event;
//   - drags the right edge of a segment: only the display moves while the mouse is down; on release an event says
//     the new length (tokens for Generate, seconds for Smooth and Linear). A length that would make the whole pass
//     30 s is shown at the most that fits, in the warning colour, and nothing is sent on release.
//
// Events: the strip commits (LLUICtrl::onCommit, so commit_callback of the XML works) and lastEvent() says what
// happened (getValue() has the same as an LLSD). The window applies it to the model (MotionTimeline::setTokens /
// setSeconds, the playback's seek) and hands the new timeline back with setTimeline(). An event is sent as the very
// last thing of a mouse handler, after the strip is back to Idle.
//
// Drag states (research/149 (R)). The machine is tlgeom::DragMachine (motiontimelinegeom.h, tested alone); the view
// only turns what happens to it into the machine's calls and does what it says:
//   Idle --press on the ruler (a Seek event at once), or on the playhead's line or its handle on the ruler, that is the
//       ruler within 5 px of the line (no Seek yet: the playhead stays)--> Playhead (not with fewer than two keys)
//   A double click is a press (the second press of a quick pair). While a drag goes on, a right or middle press is taken
//   and changes nothing (it would reach the world's menu or camera).
//   Idle --press on an editable segment's right edge (not queued or running, not setEditable(false))--> Edge
//   Playhead --mouse moves--> Playhead (a Seek event when the frame changes)
//   Playhead --release--> Idle (a Seek event if the frame changed since the last one)
//   Edge --mouse moves--> Edge (the length shown, its label and the layout change; nothing is sent)
//   Edge --release--> Idle (a Resize event if the length changed and was not refused)
//   any --Esc, focus lost, mouse capture lost, hidden, disabled--> Idle (no event)
//   Edge --setEditable(false), or a new timeline in which the segment is gone, queued, running, of another method or
//       length, or the whole's length changed--> Idle (no event; the display is the model's again)
// Out of every drag the mouse capture is let go, the keyboard focus taken for the drag goes back to whoever had it
// (directly when it is an ancestor, like the floater; the floater around the strip when it is gone, hidden or disabled), and the
// playhead (after a Playhead drag) stays where the drag put it. The width of a second is fixed for the length of an
// Edge drag (it would run away if it followed the growing whole), and the dragged segment is drawn at its true
// width (no minimum), so that its edge follows the mouse.

#ifndef PLUS_POSERMOTION_MOTIONTIMELINEVIEW_H
#define PLUS_POSERMOTION_MOTIONTIMELINEVIEW_H

#include "motiontimeline.h"
#include "motiontimelinegeom.h"

#include "plus/photoscreen/psui/pswidgets.h"

#include "llframetimer.h"

#include <array>
#include <string>
#include <vector>

namespace plus::posermotion
{
    class MotionTimelineView : public psui::PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, psui::PsCtrl::Params>
        {
            Params();
        };

        /// The words of the strip (the window reads them from its XML strings, en and ja). The defaults are English.
        struct Strings
        {
            static constexpr size_t JOB_ERRORS = 13;   // the values of JobError (the last one is PrevMissing: motiontimelineview.cpp checks)
            std::string generate = "Generate";
            std::string smooth   = "Smooth";
            std::string linear   = "Linear";
            std::string hold     = "Hold";
            std::string unmade   = "Not generated yet (a smooth stand-in plays)";
            std::string queued   = "Waiting to be generated";
            std::string running  = "Generating";
            std::string made     = "Ready";
            std::string failed   = "Failed";
            std::string regen    = "Regenerating is advised";
            std::string second   = "s";                // after a number of seconds
            std::string separator = " - ";             // in the tool tip between the length and the state
            std::string reason_prefix = ": ";          // before the reason of a failure, and before the reasons of "regen"
            std::string note_prefix   = "\n";          // before "regenerating is advised"
            /// Why a segment failed, by JobError. An empty one: the English of describe(JobError).
            std::array<std::string, JOB_ERRORS> job_error;
            /// Why regenerating is advised, by the bit of SegmentInfo::regen_reasons (REGEN_ENDS, _TARGETS, _CONTEXT, _PASS
            /// in this order), and what goes between two of them. An empty one is left out.
            static constexpr size_t REGEN_REASONS = 4;
            std::array<std::string, REGEN_REASONS> regen_reason = { "a key at its end changed", "the keys after it changed",
                                                                    "the motion before changed", "the way it passes changed" };
            std::string regen_join = ", ";
        };

        /// What the user did (the last commit).
        struct Event
        {
            enum class Kind : U8
            {
                None,
                Seek,    // the playhead: seek_sec
                Resize,  // a segment's right edge: pair, method, tokens (Generate) or seconds (Smooth, Linear)
            };
            Kind      kind     = Kind::None;
            F64       seek_sec = 0.0;
            PairId    pair;
            SegMethod method   = SegMethod::Generate;
            S32       tokens   = 0;
            F32       seconds  = 0.f;
        };

        // ---- what the window gives ---------------------------------------------------------------------
        /// The timeline to show. Cheap when nothing that is shown changed (compared first), so the window may call it on
        /// every tick; otherwise it copies (what the strip needs) and makes the labels again.
        void setTimeline(const MotionTimeline& timeline);
        /// Draws these as chosen (the window owns the selection; this does not select).
        void setSelection(KeyId key, PairId pair);
        /// The playhead, in seconds of the joined clip (ignored while the user drags it). Follows it into view.
        void setPlayheadSec(F64 seconds);
        void setStrings(const Strings& strings);
        /// "Regenerating is advised" and its reasons (the REGEN_* bits), in the words of setStrings(): the strip's tool tip
        /// and the window's line of the selected segment say it the same way. Made when asked (not for drawing).
        std::string regenText(U8 reasons) const;
        /// false: the right edges cannot be dragged (the playhead can). The window turns it off while it works.
        void setEditable(bool editable);

        // ---- what the window reads ---------------------------------------------------------------------
        const Event& lastEvent() const { return mEvent; }
        LLSD         getValue() const override;
        bool         dragging() const { return mMachine.dragging(); }
        /// The strip's height (a fixed height: the window's XML gives it as the view's height).
        static S32   bandHeight() { return (S32)tlgeom::BAND_HEIGHT; }
        /// The width of everything (margins included); the view scrolls inside its own width when it is wider.
        F32          contentWidth() const { return mLayout.contentWidth(); }

        // ---- LLView ------------------------------------------------------------------------------------
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        void setEnabled(bool enabled) override;
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMiddleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
        bool handleToolTip(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        void onMouseCaptureLost() override;
        void onFocusLost() override;
        void onVisibilityChange(bool new_visibility) override;
        ~MotionTimelineView() override;

    protected:
        friend class LLUICtrlFactory;
        explicit MotionTimelineView(const Params& p);

    private:
        struct KeyView
        {
            KeyId id          = NO_KEY;
            S32   hold_frames = 0;
            F32   hold_sec    = 0.f;
        };
        struct SegView        // what is shown of a SegmentInfo, with its words
        {
            PairId    pair;
            SegMethod method = SegMethod::Generate;
            SegState  state  = SegState::Unmade;
            JobError  fail   = JobError::None;
            S32       frames = 0;       // the intervals between its two keys
            U8        regen  = 0;       // SegmentInfo::regen_reasons: not 0 is the one mark of "regenerating is advised"
            LLWString label_full;       // "Smooth 1.5s"
            LLWString label_short;      // "Smooth"
            S32       full_width  = 0;
            S32       short_width = 0;
            S32       label_mode  = 0;  // 0 none, 1 short, 2 full: what fits the width now (layout)
        };

        bool sameAsView(const MotionTimeline& timeline) const;
        void setLabels(SegView& seg, S32 frames) const;
        const std::string& methodName(SegMethod method) const;
        const std::string& stateName(SegState state) const;
        std::string reasonOf(JobError error) const;
        void rebuildLayout(bool new_scale);
        void rebuildTicks();
        F32  viewX(F32 content_x) const { return content_x + tlgeom::MARGIN_PX - mScroll; }
        F32  contentX(S32 view_x) const { return (F32)view_x - tlgeom::MARGIN_PX + mScroll; }
        F64  playheadFrame() const;
        F64  seekSecAt(S32 view_x) const;
        void showPlayhead();
        void setScroll(F32 scroll);
        void makeTip(const tlgeom::Hit& hit);

        tlgeom::Hit hitAt(S32 x, S32 y) const;
        bool segmentEditable(S32 index) const;
        void fire(const tlgeom::DragOutcome& outcome);   // last thing of a handler
        void endDrag();                                  // a way out without an event
        void finishDrag(tlgeom::DragState was, bool relayout);
        void takeFocusForDrag();

        void drawSegment(const SegView& seg, S32 index, const tlgeom::Span& span, F32 a);
        void drawHold(const tlgeom::Span& span, F32 a);
        void drawKey(const tlgeom::Span& span, F32 a);
        void drawNeedle(F32 a);
        void drawRuler(F32 a);
        void drawScrollIndicator(F32 a);

        // ---- the copy of the timeline ----
        std::vector<KeyView> mKeys;
        std::vector<SegView> mSegs;
        S32                  mTotalIntervals = 0;       // frames - 1 of the whole as the model says
        Strings              mStrings;
        std::vector<tlgeom::Piece> mPieces;             // scratch for the layout (memory kept)

        // ---- the layout ----
        tlgeom::Layout       mLayout;
        F32                  mScroll = 0.f;
        tlgeom::TickSet        mTicks;
        std::vector<LLWString> mTickText;               // what mTicks says, as the font draws it
        std::vector<S32>       mTickWidth;

        // ---- the display state ----
        KeyId                mSelectedKey = NO_KEY;
        PairId               mSelectedPair;
        F64                  mPlayheadSec = 0.0;
        bool                 mEditable    = true;
        tlgeom::Hit          mHoverHit;                 // under the mouse (the highlight and the tool tip)
        tlgeom::Hit          mTipHit;                   // what mTipText is about
        std::string          mTipText;
        bool                 mTipValid = false;
        F64                  mFollowAfter = 0.0;        // seconds on mPulse: follow the playhead again after this
        LLFrameTimer         mPulse;                    // running segments breathe; the follow pause
        const LLFontGL*      mFont;
        LLWString            mWarn;                     // "!"

        // ---- the drag ----
        tlgeom::DragMachine  mMachine;
        bool                 mTookFocus = false;        // the keyboard focus was taken for the drag (Esc): given back
        LLHandle<LLUICtrl>   mPrevFocus;                // whose it was (it gets it back if it is still there)

        Event                mEvent;
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONTIMELINEVIEW_H
