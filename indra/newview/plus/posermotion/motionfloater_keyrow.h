/**
 * @file motionfloater_keyrow.h
 * @brief "Motion Between Poses": the row of key chips (research/357 section 2): one chip per key, in order, wrapping
 * into lines; a click selects, a drag moves a key to another place in the order, a right click asks for the menu.
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

// The row shows what the window hands over (setKeys, setSelected) and says what the user did (commit, lastEvent());
// it never changes the timeline itself. The window applies the event and hands the new keys back.
//
// Drag states (research/149 (R)):
//   Idle    --left press on a chip (mouse captured)--> Pressed
//   Pressed --moves 4 px or more--> Dragging;  --release--> Idle (a Select event)
//   Dragging --moves--> Dragging (the chip under the mouse is the place);  --release--> Idle (a Move event when the place
//            differs from where the key was)
//   any     --capture lost, hidden, disabled, the dragged key gone from setKeys()--> Idle (no event)
// Keys (the row takes the keyboard focus on a press and with Tab): Left and Right select the previous and the next key (a
// Select event); Esc during a drag puts the chip back (no event); losing the focus ends a drag too.
// A right press while Idle asks for the menu (a Menu event with the position); while a drag goes on it is taken and does
// nothing. The event is the last thing a handler does (the row is back in Idle then, the capture let go).

#ifndef PLUS_POSERMOTION_MOTIONFLOATER_KEYROW_H
#define PLUS_POSERMOTION_MOTIONFLOATER_KEYROW_H

#include "motiontimeline.h"

#include "plus/photoscreen/psui/pswidgets.h"

#include <array>
#include <vector>

namespace plus::posermotion
{
    class MotionKeyRow : public psui::PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, psui::PsCtrl::Params>
        {
            Params();
        };

        static constexpr S32 CHIP_W = 52;
        static constexpr S32 CHIP_H = 26;
        static constexpr S32 CHIP_GAP = 4;

        /// What the user did (the last commit).
        struct Event
        {
            enum class Kind : U8
            {
                None,
                Select, // a chip was clicked: id
                Move,   // a chip was dragged to another place: id, to_index (where it is to be in the order)
                Menu,   // a right click on a chip: id, and where (x, y in the row)
            };
            Kind  kind     = Kind::None;
            KeyId id       = NO_KEY;
            S32   to_index = 0;
            S32   x        = 0;
            S32   y        = 0;
        };

        /// The keys to show (a copy of their ids only; nothing is kept when they did not change).
        void setKeys(const MotionTimeline& timeline);
        /// Draws this key as chosen (the window owns the selection).
        void setSelected(KeyId id);

        const Event& lastEvent() const { return mEvent; }
        LLSD         getValue() const override { return LLSD((S32)mEvent.kind); }
        bool         dragging() const { return mState == State::Dragging; }

        void draw() override;
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseCaptureLost() override;
        void onFocusLost() override;
        void onVisibilityChange(bool new_visibility) override;
        void setEnabled(bool enabled) override;
        ~MotionKeyRow() override;

    protected:
        friend class LLUICtrlFactory;
        explicit MotionKeyRow(const Params& p);

    private:
        enum class State : U8
        {
            Idle,
            Pressed,
            Dragging,
        };

        S32  perLine() const;
        S32  chipAt(S32 x, S32 y, bool clamp) const;   // the index of the chip under (x, y); -1 none (clamped: the nearest)
        LLRectf chipRect(S32 index) const;
        void endDrag();                                // a way out without an event
        void fire(const Event& event);                 // last thing of a handler

        std::vector<KeyId> mIds;
        KeyId              mSelected = NO_KEY;
        std::array<LLWString, TIMELINE_MAX_KEYS> mLabels;
        const LLFontGL*    mFont;

        State mState   = State::Idle;
        S32   mDragIndex = -1;       // the chip pressed
        KeyId mDragId  = NO_KEY;
        S32   mPressX  = 0;
        S32   mPressY  = 0;
        S32   mMouseX  = 0;
        S32   mMouseY  = 0;
        S32   mPlace   = -1;         // Dragging: the chip under the mouse (where the key would go)
        Event mEvent;
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONFLOATER_KEYROW_H
