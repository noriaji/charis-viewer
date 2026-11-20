/**
 * @file motionfloater_keyrow.cpp
 * @brief "Motion Between Poses": the row of key chips (see motionfloater_keyrow.h).
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

#include "motionfloater_keyrow.h"

#include "plus/photoscreen/psui/psstyle.h"

#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "lluictrlfactory.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace plus::posermotion
{
    using namespace plus::psui;

    namespace
    {
        LLDefaultChildRegistry::Register<MotionKeyRow> sKeyRowRegistration("plus_motion_keys");

        constexpr S32 DRAG_START_PX = 4;   // the mouse must move this far from the press before a press is a drag
        constexpr F32 DRAG_GHOST    = 0.6f; // the opacity of the chip that follows the mouse
    } // namespace

    MotionKeyRow::Params::Params()
    {
        mouse_opaque(true);
        tab_stop(true);
    }

    MotionKeyRow::MotionKeyRow(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_SMALL))
    {
        mIds.reserve(TIMELINE_MAX_KEYS);
        for (S32 i = 0; i < TIMELINE_MAX_KEYS; ++i)
        {
            mLabels[(size_t)i] = utf8str_to_wstring(std::to_string(i + 1)); // made once: draw() builds no string
        }
    }

    MotionKeyRow::~MotionKeyRow()
    {
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
    }

    // =============================================================================================
    // what the window gives

    void MotionKeyRow::setKeys(const MotionTimeline& timeline)
    {
        const S32 count = timeline.keyCount();
        bool same = (S32)mIds.size() == count;
        for (S32 i = 0; same && i < count; ++i)
        {
            same = mIds[(size_t)i] == timeline.key(i).id;
        }
        if (same)
        {
            return;
        }
        mIds.clear();
        for (S32 i = 0; i < count && i < TIMELINE_MAX_KEYS; ++i)
        {
            mIds.push_back(timeline.key(i).id);
        }
        if (mState != State::Idle && std::find(mIds.begin(), mIds.end(), mDragId) == mIds.end())
        {
            endDrag(); // the key being dragged is gone
        }
        else if (mState != State::Idle)
        {
            mDragIndex = (S32)(std::find(mIds.begin(), mIds.end(), mDragId) - mIds.begin());
        }
    }

    void MotionKeyRow::setSelected(KeyId id)
    {
        mSelected = id;
    }

    // =============================================================================================
    // geometry

    S32 MotionKeyRow::perLine() const
    {
        return std::max(1, (getRect().getWidth() + CHIP_GAP) / (CHIP_W + CHIP_GAP));
    }

    LLRectf MotionKeyRow::chipRect(S32 index) const
    {
        const S32 per = perLine();
        const F32 x0  = (F32)((index % per) * (CHIP_W + CHIP_GAP));
        const F32 top = (F32)(getRect().getHeight() - (index / per) * (CHIP_H + CHIP_GAP));
        return LLRectf(x0, top, x0 + (F32)CHIP_W, top - (F32)CHIP_H);
    }

    S32 MotionKeyRow::chipAt(S32 x, S32 y, bool clamp) const
    {
        const S32 count = (S32)mIds.size();
        if (count == 0)
        {
            return -1;
        }
        const S32 per  = perLine();
        const S32 col  = x / (CHIP_W + CHIP_GAP);
        const S32 line = (getRect().getHeight() - y) / (CHIP_H + CHIP_GAP);
        const S32 gx   = x - col * (CHIP_W + CHIP_GAP);
        const S32 gy   = (getRect().getHeight() - y) - line * (CHIP_H + CHIP_GAP);
        if (clamp)
        {
            const S32 c = std::clamp(col, 0, per - 1);
            const S32 l = std::max(line, 0);
            return std::clamp(l * per + c, 0, count - 1);
        }
        if (x < 0 || y < 0 || col >= per || gx >= CHIP_W || gy >= CHIP_H || gy < 0)
        {
            return -1;
        }
        const S32 index = line * per + col;
        return index < count ? index : -1;
    }

    // =============================================================================================
    // drawing

    void MotionKeyRow::draw()
    {
        const F32 a = drawAlpha();
        const S32 count = (S32)mIds.size();
        for (S32 i = 0; i < count; ++i)
        {
            const bool dragged = mState == State::Dragging && i == mDragIndex;
            const bool chosen  = mIds[(size_t)i] == mSelected;
            const LLRectf r    = chipRect(i);
            const F32 alpha    = dragged ? a * 0.35f : a;
            drawRoundBordered(r, style::TL_SEG_RADIUS, Fill::solid(faded(chosen ? style::ACCENT : style::TL_FILL, alpha)),
                              faded(chosen ? style::ACCENT_LINE : style::TL_BORDER, alpha));
            drawText(mFont, mLabels[(size_t)i], (r.mLeft + r.mRight) * 0.5f, (r.mTop + r.mBottom) * 0.5f,
                     faded(style::TEXT_WHITE, alpha), LLFontGL::HCENTER, LLFontGL::VCENTER);
            if (mState == State::Dragging && i == mPlace && i != mDragIndex)
            {
                drawRing(r, style::TL_SEG_RADIUS, 2.f, faded(style::ACCENT_LINE, a));   // where the key would go
            }
        }
        if (hasFocus() && getEnabled())
        {
            drawFocusRing(localRectf(), style::TL_RADIUS);
        }
        if (mState == State::Dragging && mDragIndex >= 0 && mDragIndex < count)
        {
            const F32 cx = (F32)mMouseX;
            const F32 cy = (F32)mMouseY;
            const LLRectf g(cx - CHIP_W * 0.5f, cy + CHIP_H * 0.5f, cx + CHIP_W * 0.5f, cy - CHIP_H * 0.5f);
            drawRoundBordered(g, style::TL_SEG_RADIUS, Fill::solid(faded(style::ACCENT, a * DRAG_GHOST)),
                              faded(style::ACCENT_LINE, a * DRAG_GHOST));
            drawText(mFont, mLabels[(size_t)mDragIndex], cx, cy, faded(style::TEXT_WHITE, a * DRAG_GHOST), LLFontGL::HCENTER,
                     LLFontGL::VCENTER);
        }
    }

    // =============================================================================================
    // the mouse

    bool MotionKeyRow::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        if (!getEnabled())
        {
            return true;
        }
        if (mState != State::Idle)
        {
            endDrag(); // a press while one is going on (a double click's second press): start again
        }
        const S32 index = chipAt(x, y, false);
        if (index < 0)
        {
            return true; // empty room: nothing, and not passed on
        }
        mState     = State::Pressed;
        mDragIndex = index;
        mDragId    = mIds[(size_t)index];
        mPressX = mMouseX = x;
        mPressY = mMouseY = y;
        mPlace     = index;
        gFocusMgr.setMouseCapture(this);
        setFocus(true); // Esc cancels the drag; the arrow keys go on from the key chosen
        return true;
    }

    bool MotionKeyRow::handleDoubleClick(S32 x, S32 y, MASK mask)
    {
        return handleMouseDown(x, y, mask);
    }

    bool MotionKeyRow::handleRightMouseDown(S32 x, S32 y, MASK mask)
    {
        if (!getEnabled() || mState != State::Idle)
        {
            return true;
        }
        const S32 index = chipAt(x, y, false);
        if (index < 0)
        {
            return true;
        }
        Event event;
        event.kind = Event::Kind::Menu;
        event.id   = mIds[(size_t)index];
        event.x    = x;
        event.y    = y;
        fire(event);
        return true;
    }

    bool MotionKeyRow::handleHover(S32 x, S32 y, MASK mask)
    {
        if (mState == State::Idle || !hasMouseCapture())
        {
            return PsCtrl::handleHover(x, y, mask);
        }
        mMouseX = x;
        mMouseY = y;
        if (mState == State::Pressed && (std::abs(x - mPressX) >= DRAG_START_PX || std::abs(y - mPressY) >= DRAG_START_PX))
        {
            mState = State::Dragging;
        }
        if (mState == State::Dragging)
        {
            mPlace = chipAt(x, y, true);
        }
        return true;
    }

    bool MotionKeyRow::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (mState == State::Idle || !hasMouseCapture())
        {
            return PsCtrl::handleMouseUp(x, y, mask);
        }
        Event event;
        if (mState == State::Pressed)
        {
            event.kind = Event::Kind::Select;
            event.id   = mDragId;
        }
        else if (mPlace >= 0 && mPlace != mDragIndex)
        {
            event.kind     = Event::Kind::Move;
            event.id       = mDragId;
            event.to_index = mPlace;
        }
        endDrag();
        if (event.kind != Event::Kind::None && getEnabled())
        {
            fire(event);
        }
        return true;
    }

    void MotionKeyRow::onMouseCaptureLost()
    {
        mState     = State::Idle;
        mDragIndex = -1;
        mDragId    = NO_KEY;
        mPlace     = -1;
        PsCtrl::onMouseCaptureLost();
    }

    bool MotionKeyRow::handleKeyHere(KEY key, MASK mask)
    {
        if (!getEnabled())
        {
            return false;
        }
        if (key == KEY_ESCAPE && mState != State::Idle)
        {
            endDrag();
            return true;
        }
        if (mState == State::Idle && mask == MASK_NONE && (key == KEY_LEFT || key == KEY_RIGHT) && !mIds.empty())
        {
            const S32 count = (S32)mIds.size();
            const S32 at    = (S32)(std::find(mIds.begin(), mIds.end(), mSelected) - mIds.begin());
            const S32 next  = at >= count ? 0 : std::clamp(at + (key == KEY_RIGHT ? 1 : -1), 0, count - 1);
            if (next != at)
            {
                Event event;
                event.kind = Event::Kind::Select;
                event.id   = mIds[(size_t)next];
                fire(event);
            }
            return true;
        }
        return PsCtrl::handleKeyHere(key, mask);
    }

    void MotionKeyRow::onFocusLost()
    {
        endDrag();
        PsCtrl::onFocusLost();
    }

    void MotionKeyRow::onVisibilityChange(bool new_visibility)
    {
        if (!new_visibility)
        {
            endDrag();
        }
        PsCtrl::onVisibilityChange(new_visibility);
    }

    void MotionKeyRow::setEnabled(bool enabled)
    {
        if (!enabled)
        {
            endDrag();
        }
        PsCtrl::setEnabled(enabled);
    }

    // =============================================================================================
    // the ways out

    void MotionKeyRow::endDrag()
    {
        mState     = State::Idle;
        mDragIndex = -1;
        mDragId    = NO_KEY;
        mPlace     = -1;
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);   // onMouseCaptureLost: the state is Idle already
        }
    }

    void MotionKeyRow::fire(const Event& event)
    {
        mEvent = event;
        onCommit();    // the window's callback (it may call setKeys / setSelected; the row is Idle)
    }
} // namespace plus::posermotion
