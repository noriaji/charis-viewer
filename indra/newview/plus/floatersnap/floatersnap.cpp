/**
 * @file floatersnap.cpp
 * @brief Windows that touch move together (Shift: one alone), and snapping to the near windows only
 *        (research/387): the plus side of four hooks in LLDragHandle and two in LLFloater (canSnapTo, reshape).
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

// Charis's own (the official viewer snaps windows but keeps no group; Firestorm has none either).
//
// Windows that touch also keep their form when the layout is done again (the main window resized, the UI scale changed:
// LLFloaterView::reshape, hook gPlusFloaterViewReshape): see layoutChanging().
//
// A drag is a title bar pressed, moved and let go (LLDragHandle). The hooks (lldraghandle.cpp, llfloater.cpp):
//   gPlusDragBegin   the title bar is pressed: the windows touching the dragged one (and the ones touching those, at
//                    most MAX_GROUP) are found, once; nothing is recorded beyond the drag
//   gPlusDragSnap    a step of the drag: where the group snaps (every member's edges against the screen's edges and the
//                    windows outside the group), instead of the official snap of the one window
//   gPlusDragMoved   after the dragged window was moved: the others go the same way; when the screen pushed a
//                    window back (LLFloaterView::adjustToFitScreen) all go back by the same
//   gPlusDragEnd     the title bar is let go: the group is forgotten
//   gPlusFloaterCanSnap  LLFloater::canSnapTo(): the members of the group do not snap to each other, and (setting) a
//                    window far off on the other axis is not a place to snap to, while a window is dragged or resized
//                    (a double click on a resize bar asks without the mouse captured: every window stays a place to snap to)
//
// "Touching" is the windows' places only (floatersnapmath.h touching()): a drag started with Shift held takes the window
// alone (with PlusFloaterGroupMove at 2 it is the other way round: only with Shift held the others come along); a window
// that is minimized, hidden, docked or on a detached window is not in a group; no group is kept after the drag, so a window
// closed, moved by something else or put at another place needs nothing here. The group is also not a state that can be
// left stale: the title bar's press makes it, the title bar's release (or the mouse capture gone: the overlay's draw and
// every hook ask gFocusMgr) ends it.
//
// Not here: the dependent windows of the official code (LLFloater::addDependentFloater) that are snapped to a window of
// the group are moved by the official code with their parent (handleReshape), so they are not moved a second time; they are
// not places to snap to either (they move with the group). A dragged window that is such a dependent of a window in the group
// stays snapped to it, but while the group is put at its new places (dragMoved) the snap is lifted and put back
// (SnapTargetScope): else the official handleReshape of its parent moves it a second time.

#include "llviewerprecompiledheaders.h"

#include "floatersnap.h"
#include "floatersnapmath.h"

#include "plus.h"
#include "plus/photoscreen/psui/psstyle.h"

#include "indra_constants.h"
#include "lldraghandle.h"
#include "llresizebar.h"
#include "llresizehandle.h"
#include "llfloater.h"
#include "llcombobox.h"
#include "lluictrlfactory.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "lltrans.h"
#include "llrender2dutils.h"
#include "llview.h"
#include "llviewercontrol.h"

#include <iterator>
#include <vector>

// The hooks of the official code (lldraghandle.cpp, llfloater.cpp), set below.
extern void (*gPlusDragBegin)(LLDragHandle* handle, MASK mask);
extern void (*gPlusDragEnd)(LLDragHandle* handle);
extern bool (*gPlusDragSnap)(LLDragHandle* handle, LLFloater* floater, const LLRect& original_rect, const LLCoordGL& mouse_dir,
                             S32 margin, LLRect& new_rect, LLView*& snap_view);
extern void (*gPlusDragMoved)(LLDragHandle* handle, LLFloater* floater, const LLRect& original_rect, const LLRect& requested_rect);
extern bool (*gPlusFloaterCanSnap)(const LLFloater* floater, const LLView* other);
extern void (*gPlusFloaterViewReshape)(LLFloaterView* view, bool done);

namespace plus::floatersnap
{
    namespace
    {
        constexpr char SETTING_GROUP[]   = "PlusFloaterGroupMove";    // 0 off, 1 touching windows go together (Shift: one), 2 only with Shift held
        constexpr char SETTING_NEAR[]    = "PlusFloaterSnapNearOnly"; // snap to the windows near on the other axis only
        constexpr char SETTING_GAP[]     = "PlusFloaterSnapGap";      // pixels between windows that snapped
        constexpr char SETTING_HINT[]    = "PlusFloaterSnapHint";     // the line over the dragged window: Shift takes it alone
        constexpr char HINT_STRING[]     = "PlusFloaterSnapHint";     // strings_plus.xml
        constexpr F32  HINT_HEIGHT       = 26.f;
        constexpr F32  HINT_GAP          = 6.f;     // from the window's top edge
        constexpr F32  HINT_PAD_X        = 11.f;
        constexpr F32  HINT_TITLE_ROOM   = 34.f;    // the title bar and a margin: where the line goes inside a window at the top edge
        constexpr char SETTING_MARGIN[]  = "SnapMargin";              // the official one: how far a snap reaches

        // The drop-downs of Preferences that list a few values of a setting (panel_plus_prefs_general.xml).
        struct Choice
        {
            const char* combo;
            bool        is_gap;     // the value is a gap (clamped as gap() does), else a snap distance (as snapMargin() does)
        };
        constexpr Choice CHOICE_COMBOS[] = { { "window_snap_combobox", false }, { "window_gap_combobox", true } };

        constexpr S32 DEFAULT_MARGIN = 10;      // the official default
        constexpr int MAX_DEPENDENT_LEVELS = 4; // dependent windows of dependent windows told after a moved window
        constexpr F32 LINE_ALPHA     = 0.9f;
        constexpr S32 LINE_WIDTH     = 2;

        enum class GroupMode : S32 { Off = 0, Touching = 1, ShiftHeld = 2 };

        GroupMode groupMode()
        {
            static LLCachedControl<S32> mode(gSavedSettings, SETTING_GROUP, (S32)GroupMode::Touching);
            return (S32)mode == (S32)GroupMode::ShiftHeld ? GroupMode::ShiftHeld
                   : (S32)mode == (S32)GroupMode::Touching ? GroupMode::Touching : GroupMode::Off;
        }

        bool nearOnly()
        {
            static LLCachedControl<bool> near_only(gSavedSettings, SETTING_NEAR, true);
            return near_only;
        }

        S32 gap()
        {
            static LLCachedControl<S32> value(gSavedSettings, SETTING_GAP, 0);
            return clampGap(value);
        }

        S32 snapMargin()
        {
            static LLCachedControl<S32> value(gSavedSettings, SETTING_MARGIN, DEFAULT_MARGIN);
            return llmax((S32)value, 0);
        }

        Box boxOf(const LLRect& r) { return { r.mLeft, r.mBottom, r.mRight, r.mTop }; }

        // A window a drag can take along or be taken along: in the main window's floater view, shown, free to be moved.
        bool eligible(LLFloater* floater)
        {
            return floater && gFloaterView && floater->getParent() == gFloaterView && floater->getVisible()
                   && !floater->isMinimized() && floater->getCanDrag() && !floater->isDocked() && !floater->getHost();
        }

        // A window's snap to the window it depends on (LLFloater::setSnappedTo), lifted for a moment and put back at the end of the
        // scope (every way out), if nothing set another one meanwhile.
        class SnapTargetScope
        {
        public:
            explicit SnapTargetScope(LLFloater* floater) : mFloater(floater->getHandle()), mTarget(floater->getSnapTarget())
            {
                if (mTarget.get())
                {
                    floater->clearSnapTarget();
                    mLifted = true;
                }
            }
            // Whether a snap was lifted.
            bool lifted() const { return mLifted; }
            ~SnapTargetScope()
            {
                LLFloater* floater = mFloater.get();
                if (floater && mTarget.get() && !floater->getSnapTarget().get())
                {
                    floater->setSnapTarget(mTarget);
                }
            }
            SnapTargetScope(const SnapTargetScope&) = delete;
            SnapTargetScope& operator=(const SnapTargetScope&) = delete;

        private:
            LLHandle<LLFloater> mFloater;
            LLHandle<LLFloater> mTarget;
            bool                mLifted = false;
        };

        // A window and the place it is to be put at.
        struct Placement
        {
            LLHandle<LLFloater> floater;
            LLRect              target;
            bool                place = false;  // false: it is at its target already (only how far the screen pushed it is read)
        };

        struct State
        {
            LLHandle<LLView>                 handle;        // the title bar that holds the group's drag
            std::vector<LLHandle<LLFloater>> members;       // [0] is the dragged window; at most MAX_GROUP
            const LLView*                    member_ptrs[MAX_GROUP] = {};   // the same, to compare only (canSnap runs for every edge of every
            size_t                           member_count = 0;              // window, in every step: no handle lookups there); never read through
            bool                             active = false;
            bool                             raised = false;    // the members were put in front (at the first step, not at the press)
            S32                              busy = 0;      // inside a hook that goes over members (no end in the middle of it)
            bool                             end_pending = false;
            LLHandle<LLView>                 overlay;
            std::vector<LLFloater*>          candidates;    // scratch of beginDrag(), valid inside that call only
            std::vector<Box>                 boxes;
            std::vector<Placement>           items;         // scratch of dragMoved(): kept for the next step (no allocation then)
            std::vector<LLHandle<LLFloater>> told;          // scratch of notifyTold(): the windows to tell, kept for the next step
            LLWString                        hint;          // the line over the dragged window, made at its first draw of a drag
            const LLFontGL*                  hint_font = nullptr;   // with its font and width, made together with it
            F32                              hint_width = 0.f;
        };

        // Never destroyed: a hook or a draw late in the shutdown must not meet a destroyed state.
        State& state()
        {
            static State* s = new State();
            return *s;
        }

        void endGroup(State& s)
        {
            if (s.busy > 0)
            {
                s.end_pending = true;       // the scope that goes over the members ends it
                return;
            }
            s.active = false;
            s.raised = false;
            s.end_pending = false;
            s.members.clear();
            s.member_count = 0;
            s.items.clear();
            s.told.clear();
            s.hint.clear();
            s.hint_font = nullptr;
            s.handle = LLHandle<LLView>();
            if (LLView* overlay = s.overlay.get())
            {
                overlay->setVisible(false);
            }
        }

        class BusyScope
        {
        public:
            explicit BusyScope(State& s) : mState(s) { ++mState.busy; }
            ~BusyScope()
            {
                if (--mState.busy == 0 && mState.end_pending)
                {
                    endGroup(mState);
                }
            }
            BusyScope(const BusyScope&) = delete;
            BusyScope& operator=(const BusyScope&) = delete;

        private:
            State& mState;
        };

        // The group is held by the drag it was made for: the title bar still has the mouse. (It is the press and release
        // that make and end it; this catches a capture lost in between, an Alt+Tab, a window that was hidden.)
        bool alive(const State& s)
        {
            return s.active && !LLFloater::isQuitRequested() && s.handle.get()
                   && gFocusMgr.getMouseCapture() == static_cast<LLMouseHandler*>(s.handle.get());
        }

        // The group of this drag: made at the press of this title bar, for its window (the first member), the mouse still held.
        bool isGroupOf(const State& s, const LLDragHandle* handle, const LLFloater* floater)
        {
            return s.active && s.handle.get() == static_cast<const LLView*>(handle) && !s.members.empty()
                   && s.members[0].get() == floater && alive(s);
        }

        bool isMember(const State& s, const LLView* view)
        {
            if (!view)
            {
                return false;
            }
            for (const LLHandle<LLFloater>& member : s.members)
            {
                if (member.get() == view)
                {
                    return true;
                }
            }
            return false;
        }

        // A window the group moves itself: not a dependent window snapped to a window of the group (the official code
        // moves that one together with its parent).
        bool movedByGroup(const State& s, LLFloater* member)
        {
            LLFloater* dependee = member->getDependee();
            return !(dependee && member->getSnapTarget().get() == dependee && isMember(s, dependee));
        }

        // Sets a window's rectangle for a moment, as the official drag does (LLDragHandle::handleHover) to ask where it snaps.
        class RectScope
        {
        public:
            RectScope(LLView* view, const LLRect& temporary) : mView(view), mOriginal(view->getRect()) { view->setRect(temporary); }
            ~RectScope() { mView->setRect(mOriginal); }
            RectScope(const RectScope&) = delete;
            RectScope& operator=(const RectScope&) = delete;

        private:
            LLView* mView;
            LLRect  mOriginal;
        };

        // The snap of a window: along each axis whether an edge was caught and how far to move (AxisSnap), and what it was
        // caught by (the tag: a LLView*, the screen's view or another window).
        struct Snap
        {
            AxisSnap x;
            AxisSnap y;
        };

        // What the window's edges are caught by, for LLFloater::setSnappedTo(): as the official snap, the last edge that caught.
        LLView* caughtBy(const Snap& snap)
        {
            return static_cast<LLView*>(snap.y.tag ? snap.y.tag : snap.x.tag);
        }

        // One edge of the window (at its rectangle now): caught by something? An edge that is in line is caught, distance 0.
        AxisSnap edgeSnap(LLFloater* floater, const LLCoordGL& mouse_dir, LLView::ESnapEdge edge, LLView::ESnapType type,
                          S32 margin, S32 padding)
        {
            const LLRect rect = floater->getRect();
            S32 value = edge == LLView::SNAP_LEFT ? rect.mLeft : edge == LLView::SNAP_RIGHT ? rect.mRight
                        : edge == LLView::SNAP_TOP ? rect.mTop : rect.mBottom;
            const S32 before = value;
            LLView* caught = floater->findSnapEdge(value, mouse_dir, edge, type, margin, padding);
            AxisSnap result;
            result.hit = caught != nullptr;
            result.distance = value - before;
            result.tag = caught;
            return result;
        }

        // The edges of one kind as LLView::findSnapRect() asks them: the right and the top edge unless the mouse goes left or
        // down, the left and the bottom edge unless it goes right or up (both when it stands: the nearer catch).
        Snap snapEdges(LLFloater* floater, const LLCoordGL& mouse_dir, LLView::ESnapType type, S32 margin, S32 padding)
        {
            Snap result;
            if (mouse_dir.mX >= 0)
            {
                result.x = nearerSnap(result.x, edgeSnap(floater, mouse_dir, LLView::SNAP_RIGHT, type, margin, padding));
            }
            if (mouse_dir.mX <= 0)
            {
                result.x = nearerSnap(result.x, edgeSnap(floater, mouse_dir, LLView::SNAP_LEFT, type, margin, padding));
            }
            if (mouse_dir.mY >= 0)
            {
                result.y = nearerSnap(result.y, edgeSnap(floater, mouse_dir, LLView::SNAP_TOP, type, margin, padding));
            }
            if (mouse_dir.mY <= 0)
            {
                result.y = nearerSnap(result.y, edgeSnap(floater, mouse_dir, LLView::SNAP_BOTTOM, type, margin, padding));
            }
            return result;
        }

        // Where the window (at its rectangle now) snaps. With a gap: the screen's edges are asked without it (findSnapEdge puts
        // the gap on the screen's edges too), the windows' with it, the nearer catch wins per axis.
        Snap snapOne(LLFloater* floater, const LLCoordGL& mouse_dir, S32 margin, S32 gap_pixels)
        {
            if (gap_pixels == 0)
            {
                return snapEdges(floater, mouse_dir, LLView::SNAP_PARENT_AND_SIBLINGS, margin, 0);
            }
            const Snap parent_snap = snapEdges(floater, mouse_dir, LLView::SNAP_PARENT, margin, 0);
            const Snap sibling_snap = snapEdges(floater, mouse_dir, LLView::SNAP_SIBLINGS, margin, gap_pixels);
            Snap result;
            result.x = nearerSnap(parent_snap.x, sibling_snap.x);
            result.y = nearerSnap(parent_snap.y, sibling_snap.y);
            return result;
        }

        //---------------------------------------------------------------------------------------------
        // The window moved by a drag: listeners (a popup that belongs to a window that moves closes, say)

        std::vector<void (*)(LLFloater*)>& movedListeners()
        {
            static std::vector<void (*)(LLFloater*)>* listeners = new std::vector<void (*)(LLFloater*)>();
            return *listeners;
        }

        void notifyOne(LLFloater* floater)
        {
            for (void (*listener)(LLFloater*) : movedListeners())
            {
                listener(floater);
            }
        }

        // The windows a drag moved are collected (addTold, each once), then told with the dependent windows snapped to them,
        // which the official code moves with their parent (LLFloater::handleReshape). The handles are collected first and
        // told after: a listener runs while nothing walks the floater view's list. Nothing is done when nobody listens.
        void addTold(State& s, LLFloater* floater)
        {
            if (!floater || movedListeners().empty())
            {
                return;
            }
            for (const LLHandle<LLFloater>& known : s.told)
            {
                if (known.get() == floater)
                {
                    return;
                }
            }
            s.told.push_back(floater->getHandle());
        }

        void notifyTold(State& s)
        {
            if (s.told.empty())
            {
                return;
            }
            if (gFloaterView)
            {
                // The dependent windows of the windows told, and theirs in turn, until none is new (a few levels at most).
                size_t known = 0;
                for (int level = 0; level < MAX_DEPENDENT_LEVELS && known < s.told.size(); ++level)
                {
                    const size_t checked = s.told.size();
                    for (LLView* child : *gFloaterView->getChildList())
                    {
                        LLFloater* dependent = dynamic_cast<LLFloater*>(child);
                        LLFloater* parent = dependent ? dependent->getDependee() : nullptr;
                        if (!parent || dependent->getSnapTarget().get() != parent)
                        {
                            continue;
                        }
                        for (size_t i = known; i < checked; ++i)
                        {
                            if (s.told[i].get() == parent)
                            {
                                addTold(s, dependent);
                                break;
                            }
                        }
                    }
                    known = checked;
                }
            }
            for (size_t i = 0; i < s.told.size(); ++i)
            {
                if (LLFloater* floater = s.told[i].get())
                {
                    notifyOne(floater);
                }
            }
            s.told.clear();
        }

        //---------------------------------------------------------------------------------------------
        // The outline of the group while it is dragged

        class OutlineView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit OutlineView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                State& s = state();
                if (!alive(s))
                {
                    endGroup(s);
                    setVisible(false);
                    return;
                }
                LLColor4 color = plus::psui::style::ACCENT_LINE;
                color.mV[VALPHA] = LINE_ALPHA;
                for (const LLHandle<LLFloater>& member : s.members)
                {
                    LLFloater* floater = member.get();
                    if (!eligible(floater))
                    {
                        continue;
                    }
                    LLRect local;
                    screenRectToLocal(floater->calcScreenRect(), &local);
                    for (S32 i = 0; i < LINE_WIDTH; ++i)
                    {
                        gl_rect_2d(local.mLeft + i, local.mTop - i, local.mRight - i, local.mBottom + i, color, false);
                    }
                }
                drawHint(s);
            }

        private:
            // While windows move together (a group exists only then, so Shift is not held): a line over the dragged window that
            // says Shift, held as the title bar is pressed, takes it alone. Not before the drag has begun (a click shows nothing); a setting turns it off.
            void drawHint(State& s)
            {
                static LLCachedControl<bool> wanted(gSavedSettings, SETTING_HINT, true);
                if (!wanted || !s.raised || groupMode() != GroupMode::Touching || s.members.empty())
                {
                    return;
                }
                LLFloater* grabbed = s.members[0].get();
                if (!eligible(grabbed))
                {
                    return;
                }
                namespace style = plus::psui::style;
                const LLRect& own = getRect();
                if (s.hint.empty() || !s.hint_font)
                {
                    s.hint = utf8str_to_wstring(LLTrans::getString(HINT_STRING));
                    s.hint_font = plus::psui::font(style::FONT_SMALL);
                    s.hint_width = s.hint_font->getWidthF32(s.hint.c_str()) + 2.f * HINT_PAD_X;
                }
                const LLFontGL* font = s.hint_font;
                const F32 width = llmin(s.hint_width, llmax(1.f, (F32)own.getWidth() - 8.f));   // a narrow screen cuts the text
                LLRect local;
                screenRectToLocal(grabbed->calcScreenRect(), &local);
                // The room over the window ends at the floater view's top (the menu and tool bars are over it), not at the screen's.
                LLRect area;
                screenRectToLocal(gFloaterView->calcScreenRect(), &area);
                const F32 left = llclamp((F32)local.mLeft, 4.f, llmax(4.f, (F32)own.getWidth() - width - 4.f));
                F32 bottom = (F32)local.mTop + HINT_GAP;
                if (bottom + HINT_HEIGHT > (F32)area.mTop)
                {
                    bottom = (F32)local.mTop - HINT_TITLE_ROOM - HINT_HEIGHT;       // no room over the window: inside, under its title bar
                }
                plus::psui::drawRoundBordered(LLRectf(left, bottom + HINT_HEIGHT, left + width, bottom), style::TOOLTIP_RADIUS,
                                              plus::psui::Fill::solid(style::TOOLTIP_FILL), style::TOOLTIP_BORDER);
                plus::psui::drawText(font, s.hint, left + HINT_PAD_X, bottom + HINT_HEIGHT * 0.5f, style::TEXT_WHITE,
                                     LLFontGL::LEFT, LLFontGL::VCENTER, (S32)(width - 2.f * HINT_PAD_X), true);
            }
        };

        //---------------------------------------------------------------------------------------------
        // The hooks

        // A title bar is pressed (mask: the keys then): the group of the drag that may follow.
        void beginDrag(LLDragHandle* handle, MASK mask)
        {
            State& s = state();
            if (s.busy > 0)
            {
                return;     // a press inside a step of a drag (a listener's doing): the group goes on
            }
            endGroup(s);
            const GroupMode mode = groupMode();
            // Touching: Shift drags the window alone. ShiftHeld: only Shift takes the others.
            if (mode == GroupMode::Off || ((mode == GroupMode::Touching) == ((mask & MASK_SHIFT) != 0)))
            {
                return;
            }
            LLFloater* grabbed = handle ? dynamic_cast<LLFloater*>(handle->getParent()) : nullptr;
            if (!eligible(grabbed))
            {
                return;
            }
            s.candidates.clear();
            s.boxes.clear();
            S32 start = -1;
            for (LLView* child : *gFloaterView->getChildList())
            {
                LLFloater* floater = dynamic_cast<LLFloater*>(child);
                if (!eligible(floater))
                {
                    continue;
                }
                if (floater == grabbed)
                {
                    start = (S32)s.candidates.size();
                }
                s.candidates.push_back(floater);
                s.boxes.push_back(boxOf(floater->getRect()));
            }
            int picked[MAX_GROUP];
            const int count = start < 0 ? 0 : collectGroup(s.boxes.data(), nullptr, (int)s.boxes.size(), start, gap(), MAX_GROUP, picked);
            if (count >= 2)
            {
                s.members.clear();
                s.member_count = 0;
                for (int i = 0; i < count; ++i)
                {
                    s.members.push_back(s.candidates[picked[i]]->getHandle());
                    s.member_ptrs[s.member_count++] = s.candidates[picked[i]];
                }
                s.handle = handle->getHandle();
                s.active = true;
                s.raised = false;
                if (LLView* overlay = s.overlay.get())
                {
                    overlay->setVisible(true);
                }
            }
            s.candidates.clear();   // pointers valid inside this call only
            s.boxes.clear();
        }

        // The others in front of the rest, the dragged window in front of them (it has the focus since the press; it is not given
        // again). At the first step of the drag, not at the press: a press that does not drag changes no order, and the press
        // is being handled by the floater view's loop over its children.
        void raiseGroup(State& s)
        {
            if (s.raised)
            {
                return;
            }
            s.raised = true;
            // Not given the focus and not restored (a minimized window is not a member; one that was minimized since stays so).
            for (size_t i = s.members.size(); i-- > 0;)
            {
                LLFloater* member = s.members[i].get();
                if (eligible(member))
                {
                    gFloaterView->bringToFront(member, false, false);
                }
            }
        }

        // A title bar is let go. Another title bar's release (a button pressed elsewhere and let go over this one) leaves a group
        // whose own title bar still has the mouse alone; one whose mouse is gone is ended by alive() at the next hook or draw.
        void endDrag(LLDragHandle* handle)
        {
            State& s = state();
            if (s.handle.get() == static_cast<LLView*>(handle) || !alive(s))
            {
                endGroup(s);
            }
        }

        // A step of the drag, the dragged window at the place the mouse brought it (a moment): where does it snap?
        bool dragSnap(LLDragHandle* handle, LLFloater* floater, const LLRect& original_rect, const LLCoordGL& mouse_dir,
                      S32 official_margin, LLRect& new_rect, LLView*& snap_view)
        {
            State& s = state();
            const S32 margin = snapMargin();
            const S32 gap_pixels = gap();
            const bool group = isGroupOf(s, handle, floater);
            if (s.active && !group)
            {
                endGroup(s);        // another drag's, or its mouse is gone
            }
            if (!group && gap_pixels == 0 && margin == official_margin)
            {
                return false;       // nothing to change: the official snap
            }
            BusyScope busy(s);
            if (group)
            {
                raiseGroup(s);
            }
            const LLRect translated = floater->getRect();
            const Snap own = snapOne(floater, mouse_dir, margin, gap_pixels);
            Snap total = own;
            LLView* caught = caughtBy(own);
            if (group)
            {
                const S32 step_x = translated.mLeft - original_rect.mLeft;
                const S32 step_y = translated.mBottom - original_rect.mBottom;
                for (size_t i = 1; i < s.members.size(); ++i)
                {
                    LLFloater* member = s.members[i].get();
                    if (!eligible(member) || !movedByGroup(s, member))
                    {
                        continue;
                    }
                    LLRect moved = member->getRect();
                    moved.translate(step_x, step_y);
                    RectScope scope(member, moved);
                    const Snap other = snapOne(member, mouse_dir, margin, gap_pixels);
                    total.x = nearerSnap(total.x, other.x);
                    total.y = nearerSnap(total.y, other.y);
                }
                // What the dragged window is snapped to counts only when its own snap is what the group does.
                const bool own_applies = (!own.x.hit || own.x.distance == total.x.distance)
                                         && (!own.y.hit || own.y.distance == total.y.distance);
                caught = own_applies ? caughtBy(own) : nullptr;
                // A dependent window of the official code snapped to its parent, the parent in the group: it stays snapped to it
                // (the parent is not a place it snaps to now, so it would come off).
                LLFloater* parent = floater->getDependee();
                if (parent && floater->getSnapTarget().get() == parent && isMember(s, parent))
                {
                    caught = parent;
                }
            }
            new_rect = translated;
            new_rect.translate(total.x.hit ? total.x.distance : 0, total.y.hit ? total.y.distance : 0);
            snap_view = caught;
            return true;
        }

        // Puts the windows at their targets (setShape as the user's: it is saved and the screen fits it in), and when the
        // screen pushed any of them back (an edge of the screen, a toolbar: LLFloaterView::adjustToFitScreen), moves all the
        // others the same way, so the form of the group is kept: all go back by the most any was. At most MAX_GROUP.
        void placeTogether(Placement* items, size_t count)
        {
            count = std::min(count, (size_t)MAX_GROUP);
            S32 push_x[MAX_GROUP] = {};
            S32 push_y[MAX_GROUP] = {};
            for (size_t i = 0; i < count; ++i)
            {
                LLFloater* floater = items[i].floater.get();
                if (!floater)
                {
                    continue;
                }
                if (items[i].place)
                {
                    floater->setShape(items[i].target, true);
                }
                if (LLFloater* placed = items[i].floater.get())     // asked again: setShape runs the window's own handlers
                {
                    push_x[i] = placed->getRect().mLeft - items[i].target.mLeft;
                    push_y[i] = placed->getRect().mBottom - items[i].target.mBottom;
                }
            }
            const S32 common_x = strongestPush(push_x, (int)count);
            const S32 common_y = strongestPush(push_y, (int)count);
            if (common_x == 0 && common_y == 0)
            {
                return;
            }
            for (size_t i = 0; i < count; ++i)
            {
                LLFloater* floater = items[i].floater.get();
                const S32 fix_x = common_x - push_x[i];
                const S32 fix_y = common_y - push_y[i];
                if (floater && (fix_x != 0 || fix_y != 0))
                {
                    LLRect target = floater->getRect();
                    target.translate(fix_x, fix_y);
                    floater->setShape(target, true);
                }
            }
        }

        // The dragged window was moved to requested_rect (and the screen may have pushed it back): the others follow.
        void dragMoved(LLDragHandle* handle, LLFloater* floater, const LLRect& original_rect, const LLRect& requested_rect)
        {
            State& s = state();
            const bool group = s.busy == 0 && isGroupOf(s, handle, floater);
            if (!group)
            {
                if (s.active && s.busy == 0 && !alive(s))
                {
                    endGroup(s);
                }
                addTold(s, floater);
                notifyTold(s);
                return;
            }
            BusyScope busy(s);
            const S32 step_x = requested_rect.mLeft - original_rect.mLeft;
            const S32 step_y = requested_rect.mBottom - original_rect.mBottom;
            // [0] is the dragged window, at its place already; the others are put a step further.
            s.items.clear();
            Placement dragged;
            dragged.floater = s.members[0];
            dragged.target = requested_rect;
            s.items.push_back(dragged);
            for (size_t i = 1; i < s.members.size(); ++i)
            {
                LLFloater* member = s.members[i].get();
                if (!eligible(member) || !movedByGroup(s, member))
                {
                    continue;
                }
                Placement other;
                other.floater = s.members[i];
                other.target = member->getRect();
                other.target.translate(step_x, step_y);
                other.place = true;
                s.items.push_back(other);
            }
            {
                // The dragged window may be snapped to its parent in the group: while the parent is put at its place, the
                // official code would move the dragged window with it again.
                LLFloater* parent = floater->getSnapTarget().get();
                const bool parent_in_group = parent && floater->getDependee() == parent && isMember(s, parent);
                SnapTargetScope lifted(floater);
                if (lifted.lifted() && parent_in_group && gFloaterView)
                {
                    // Without its snap the official fit of the parent (which takes its snapped dependents into the rectangle it
                    // keeps on the screen) does not include it: it is fitted on its own here, before the push it got is measured,
                    // so all go back together by the most any was pushed (a toolbar, the edge of the screen).
                    gFloaterView->adjustToFitScreen(floater, !floater->isMinimized());
                }
                placeTogether(s.items.data(), s.items.size());
            }
            for (const Placement& item : s.items)
            {
                addTold(s, item.floater.get());
            }
            notifyTold(s);
        }

        //---------------------------------------------------------------------------------------------
        // The main window or the UI scale changes: the touching windows keep their form

        struct Remembered
        {
            LLHandle<LLFloater> floater;
            LLRect              rect;
        };

        std::vector<Remembered>& remembered()
        {
            static std::vector<Remembered>* list = new std::vector<Remembered>();
            return *list;
        }

        bool sameSize(const LLRect& a, const LLRect& b) { return a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight(); }

        // LLFloaterView::reshape() lays every window out again by its relative place (applyPositioning): done false before,
        // true after. Windows that touched before are put back together after: the first of each group stays where the
        // layout put it, the others at their distance from it from before. Not a window whose size changed, a window moved
        // with its parent (a dependent one), a window that is not free to move; only with PlusFloaterGroupMove at 1 (the
        // group of a drag is a thing of the drag: with 0 or 2 touching windows are left as the layout puts them).
        void layoutChanging(LLFloaterView* view, bool done)
        {
            std::vector<Remembered>& list = remembered();
            if (view != gFloaterView)
            {
                return;
            }
            if (!done)
            {
                list.clear();
                if (groupMode() != GroupMode::Touching)
                {
                    return;
                }
                for (LLView* child : *view->getChildList())
                {
                    LLFloater* floater = dynamic_cast<LLFloater*>(child);
                    if (eligible(floater) && !floater->isDependent())
                    {
                        list.push_back({ floater->getHandle(), floater->getRect() });
                    }
                }
                if (list.size() < 2)
                {
                    list.clear();
                }
                return;
            }
            if (list.empty())
            {
                return;
            }
            std::vector<Box> boxes;
            boxes.reserve(list.size());
            for (const Remembered& r : list)
            {
                boxes.push_back(boxOf(r.rect));
            }
            std::vector<char> seen(list.size(), 0);
            std::vector<Placement> items;
            std::vector<int> picked(list.size());
            for (size_t start = 0; start < list.size(); ++start)
            {
                if (seen[start])
                {
                    continue;
                }
                // The whole set of touching windows is taken as seen, though only the first MAX_GROUP are put (the rest is not
                // started over from, which would put some of them twice).
                const int whole = collectGroup(boxes.data(), nullptr, (int)boxes.size(), (int)start, gap(), (int)boxes.size(), picked.data());
                for (int i = 0; i < whole; ++i)
                {
                    seen[picked[i]] = 1;
                }
                const int count = std::min(whole, MAX_GROUP);
                // The anchor is the first window of the set that is still there and has the size it had; it stays where the layout
                // put it, the others at their distance from it from before.
                int anchor_at = -1;
                for (int i = 0; i < count && anchor_at < 0; ++i)
                {
                    LLFloater* candidate = list[picked[i]].floater.get();
                    if (eligible(candidate) && sameSize(candidate->getRect(), list[picked[i]].rect))
                    {
                        anchor_at = i;
                    }
                }
                if (count < 2 || anchor_at < 0)
                {
                    continue;
                }
                const Remembered& first = list[picked[anchor_at]];
                const LLRect first_before = first.rect;
                const LLRect first_now = first.floater.get()->getRect();
                items.clear();
                Placement anchor;
                anchor.floater = first.floater;
                anchor.target = first_now;
                items.push_back(anchor);
                for (int i = 0; i < count; ++i)
                {
                    LLFloater* other = i == anchor_at ? nullptr : list[picked[i]].floater.get();
                    if (!eligible(other) || !sameSize(other->getRect(), list[picked[i]].rect))
                    {
                        continue;
                    }
                    const LLRect before = list[picked[i]].rect;
                    Placement placement;
                    placement.floater = list[picked[i]].floater;
                    placement.target = other->getRect();
                    placement.target.translate(first_now.mLeft + (before.mLeft - first_before.mLeft) - placement.target.mLeft,
                                               first_now.mBottom + (before.mBottom - first_before.mBottom) - placement.target.mBottom);
                    placement.place = true;
                    items.push_back(placement);
                }
                placeTogether(items.data(), items.size());
            }
            list.clear();
        }

        // Whether the mouse is held by a title bar, a resize bar or a resize handle: a window is being dragged or resized. (A double
        // click on a resize bar spreads the window to the nearest edge by asking with no limit on the distance, and no capture.)
        // The answer is kept for the captor and the frame it was asked in: canSnap asks for every edge of every window.
        bool draggingOrResizing()
        {
            static LLMouseHandler* sCaptor = nullptr;
            static U32 sFrame = 0;
            static bool sAnswer = false;
            LLMouseHandler* captor = gFocusMgr.getMouseCapture();
            if (!captor)
            {
                sCaptor = nullptr;
                return false;
            }
            if (captor != sCaptor || sFrame != LLFrameTimer::getFrameCount())
            {
                sCaptor = captor;
                sFrame = LLFrameTimer::getFrameCount();
                sAnswer = dynamic_cast<LLDragHandle*>(captor) || dynamic_cast<LLResizeBar*>(captor)
                          || dynamic_cast<LLResizeHandle*>(captor);
            }
            return sAnswer;
        }

        // The place of view among the members of the group, or -1. The pointers are compared first (canSnap runs for every edge
        // of every window); a match is checked with the handle too, since a window closed during the drag may have given its
        // address to another.
        int memberIndex(const State& s, const LLView* view)
        {
            for (size_t i = 0; i < s.member_count; ++i)
            {
                if (s.member_ptrs[i] == view)
                {
                    return s.members[i].get() == view ? (int)i : -1;
                }
            }
            return -1;
        }

        // LLFloater::canSnapTo(): false when the window must not snap to other.
        bool canSnap(const LLFloater* floater, const LLView* other)
        {
            State& s = state();
            if (s.member_count > 0 && memberIndex(s, floater) >= 0 && alive(s))
            {
                // Not to the other members of the group, and not to the dependent windows snapped to a member (the official code
                // moves them with it): all of them go with the group, they are not at rest.
                if (memberIndex(s, other) >= 0)
                {
                    return false;
                }
                const LLFloater* dependent = dynamic_cast<const LLFloater*>(other);
                LLFloater* parent = dependent ? const_cast<LLFloater*>(dependent)->getDependee() : nullptr;
                if (parent && dependent->getSnapTarget().get() == parent && memberIndex(s, parent) >= 0)
                {
                    return false;
                }
            }
            if (!nearOnly())
            {
                return true;
            }
            if (!floater || !dynamic_cast<const LLFloater*>(other) || floater->getParent() != gFloaterView
                || other->getParent() != gFloaterView || !draggingOrResizing())
            {
                return true;
            }
            return nearEnough(boxOf(floater->getRect()), boxOf(other->getRect()), snapMargin() + gap());
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusFloaterGroupMove, PlusFloaterSnapNearOnly, PlusFloaterSnapGap);
            // Preferences > Charis > General > Windows (panel_plus_prefs_general.xml).
            gPlusDragBegin      = &beginDrag;
            gPlusDragEnd        = &endDrag;
            gPlusDragSnap       = &dragSnap;
            gPlusDragMoved      = &dragMoved;
            gPlusFloaterCanSnap = &canSnap;
            gPlusFloaterViewReshape = &layoutChanging;
        }

        // Preferences > Charis > General: the drop-downs of snap distance and gap list a few values. A value that is not in the
        // list (the debug settings, an older file) is kept: this view (in panel_plus_prefs_general.xml, as large as a point, drawn
        // only while the page is shown) gives the drop-down a "Custom (N)" item of it, chosen. Nothing is written to the settings.
        // The values are read again at once when the page is shown again and every REFRESH_SECONDS while it is.
        class ChoicesView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit ChoicesView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                const U32 frame = LLFrameTimer::getFrameCount();
                // Not drawn in the frame before: the page was shown again (the same frame twice is not that).
                const bool shown_again = frame != mLastFrame && frame != mLastFrame + 1;
                mLastFrame = frame;
                if (!shown_again && mTimer.getElapsedTimeF32() < REFRESH_SECONDS)
                {
                    return;
                }
                mTimer.reset();
                LLView* page = getParent();
                for (size_t i = 0; i < std::size(CHOICE_COMBOS) && page; ++i)
                {
                    showCustomValue(page->findChild<LLComboBox>(CHOICE_COMBOS[i].combo, true), CHOICE_COMBOS[i].is_gap, mCustom[i],
                                    shown_again);
                }
            }

        private:
            struct Custom
            {
                std::string label;          // empty: no custom item now
                S32         value = 0;      // the value of the custom item
                bool        seen = false;   // the setting's value below was handled
                S32         seen_value = 0;
            };

            // The item that shows the value the setting really has: the value as it is used (snapMargin(), gap()), a listed
            // item when there is one, else a custom item. Nothing is touched while the setting has not changed since the last
            // time (the open list's chosen row stays where the user moved it) and while the combo's list is open (the list has the keyboard focus).
            static void showCustomValue(LLComboBox* combo, bool is_gap, Custom& custom, bool again)
            {
                LLControlVariable* control = combo ? combo->getControlVariable() : nullptr;
                if (!control)
                {
                    return;
                }
                const S32 raw = control->getValue().asInteger();
                if (!again && custom.seen && custom.seen_value == raw)
                {
                    return;
                }
                if (gFocusMgr.childHasKeyboardFocus(combo))
                {
                    return;             // its list is open: tried again at the next refresh
                }
                custom.seen = true;
                custom.seen_value = raw;
                const S32 value = is_gap ? clampGap(raw) : llmax(raw, 0);
                if (!custom.label.empty())
                {
                    if (value == custom.value)
                    {
                        return;         // still the same custom value
                    }
                    combo->remove(custom.label);
                    custom.label.clear();
                }
                if (combo->setSelectedByValue(LLSD(value), true))
                {
                    combo->setValue(LLSD(value));   // also what closing the list (Esc, a click outside) goes back to
                    return;             // one of the listed values
                }
                LLStringUtil::format_map_t args;
                args["[VALUE]"] = std::to_string(value);
                custom.label = LLTrans::getString("PlusFloaterSnapCustom", args);
                custom.value = value;
                combo->add(custom.label, LLSD(value));
                combo->setValue(LLSD(value));
            }

            static constexpr F32 REFRESH_SECONDS = 0.5f;

            LLFrameTimer mTimer;
            U32          mLastFrame = 0;
            Custom       mCustom[std::size(CHOICE_COMBOS)];
        };

        LLDefaultChildRegistry::Register<ChoicesView> sChoicesRegistration("plus_snap_choices");

        void onUIReady()
        {
            plus::psui::attachWorldOverlay<OutlineView>(state().overlay, "plus_floater_snap_outline", false,
                                                        plus::psui::OverlayOrder::Front);
        }

        plus::FeatureRegistration sRegistration({ "floatersnap", &init, &onUIReady });
    }

    bool isGroupDrag(const LLMouseHandler* captor)
    {
        const State& s = state();
        return captor && s.active && alive(s) && static_cast<const LLMouseHandler*>(s.handle.get()) == captor;
    }

    // For a feature whose view belongs to a window and must follow or close when it is dragged (the popups that go out of a
    // window, plus/rmlui). Listeners are added at the start of the program and kept for good (there is no way to take one
    // away, and no context pointer: it is one function for all windows, which finds its own window from the one it is told
    // about). Called for every window a title bar drag moved (the dragged one, the others of its group) and for the dependent
    // windows snapped to them, which the official code moves with their parent, each once per step, after they were moved.
    void addMovedListener(void (*listener)(LLFloater*))
    {
        if (listener)
        {
            movedListeners().push_back(listener);
        }
    }
}
