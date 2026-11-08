/**
 * @file pscompat.cpp
 * @brief The official widgets in the photo screen's look, and the looks the psui widgets share (research/266 2.1).
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

#include "pscompat.h"
#include "psstyle.h"

#include "llflashtimer.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llslider.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewborder.h"

#include <cmath>

namespace plus::psui
{
    namespace
    {
        LLDefaultChildRegistry::Register<PsButtonCompat> sButtonCompatRegistration("plus_ps_button_compat");
        LLDefaultChildRegistry::Register<PsCheckCompat>  sCheckCompatRegistration("plus_ps_check_compat");
        LLDefaultChildRegistry::Register<PsSliderCompat> sSliderCompatRegistration("plus_ps_slider_compat");
        LLDefaultChildRegistry::Register<PsSpinner>      sSpinnerRegistration("plus_ps_spinner");
        LLDefaultChildRegistry::Register<PsText>         sTextRegistration("plus_ps_text");
        LLDefaultChildRegistry::Register<PsRadio>        sRadioRegistration("plus_ps_radio");

        const LLWString EMPTY_TEXT;

        const LLWString& wtextOf(const LLTextBox* text)
        {
            return text ? text->getWText() : EMPTY_TEXT;
        }

        bool highlighted(const LLTextBox* text)
        {
            return text && text->ll::ui::SearchableControl::getHighlighted();
        }

        LLRect toRect(const LLRectf& r)
        {
            return LLRect(S32(r.mLeft), S32(r.mTop), S32(r.mRight), S32(r.mBottom));
        }

        // The mouse over view (in its coordinates), only while it is (mHover of the widgets: no other view in front).
        bool mouseIn(const LLView& view, const LLRect& rect)
        {
            S32 x = 0, y = 0;
            LLUI::getInstance()->getMousePositionLocal(&view, &x, &y);
            return rect.pointInRect(x, y);
        }

        // The official value editor of a slider or spinner as PsSlider's: no box of its own (the value box is drawn
        // under it), white text with the body font. It is drawn only while it has the focus.
        void styleValueEditor(LLLineEditor* editor)
        {
            if (!editor)
            {
                return;
            }
            editor->setBgImage(nullptr);
            editor->setBgImageFocused(nullptr);
            const LLUIColor white(style::TEXT_WHITE);
            editor->setFgColor(white);
            editor->setReadOnlyFgColor(white);
            editor->setTentativeFgColor(white);
            editor->setCursorColor(white);
            editor->setFont(font(style::FONT_BODY));
            editor->setTextPadding(style::VALUE_PAD, style::VALUE_PAD);
            for (LLView* child : *editor->getChildList())
            {
                if (LLViewBorder* border = dynamic_cast<LLViewBorder*>(child))
                {
                    border->setVisible(false);
                }
            }
        }

        // While the official text draws, its own search highlight (SearchableControlHighlight*) is off: the look's is drawn.
        class HighlightOff
        {
        public:
            explicit HighlightOff(const ll::ui::SearchableControl& ctrl)
            :   mCtrl(ctrl),
                mWas(ctrl.getHighlighted())
            {
                if (mWas)
                {
                    mCtrl.setHighlighted(false);
                }
            }
            ~HighlightOff()
            {
                if (mWas)
                {
                    mCtrl.setHighlighted(true);
                }
            }
            HighlightOff(const HighlightOff&) = delete;
            HighlightOff& operator=(const HighlightOff&) = delete;

        private:
            const ll::ui::SearchableControl& mCtrl;
            const bool mWas;
        };
    }

    // =========================================================================================
    // the looks

    namespace look
    {
        void check(const LLRectf& r, bool on, bool hover, bool down, F32 alpha, const LLFontGL* font,
                   const LLWString& label, const LLUIImage* mark, const LLUICtrl& focus)
        {
            const F32 b = F32(style::CHECK_BOX);
            const F32 by = r.mBottom + std::floor((r.getHeight() - b) * 0.5f);
            const LLRectf box(r.mLeft, by + b, r.mLeft + b, by);
            if (on)
            {
                const LLColor4 fill = down ? pressed(style::CHECK_ON) : hover ? hovered(style::CHECK_ON) : style::CHECK_ON;
                drawRound(box, style::CHECK_RADIUS, Fill::solid(faded(fill, alpha)));
                drawIcon(mark, box.getCenterX(), by + b * 0.5f, style::CHECK_MARK, faded(style::CHECK_MARK_COLOR, alpha));
            }
            else
            {
                drawRoundBordered(box, style::CHECK_RADIUS, Fill::solid(faded(style::CHECK_OFF, alpha)),
                                  faded(hover ? style::CHECK_BORDER_HOVER : style::CHECK_BORDER, alpha));
            }
            const F32 x = box.mRight + F32(style::CHECK_GAP);
            drawText(font, label, x, r.mBottom + r.getHeight() * 0.5f, faded(style::CHECK_TEXT, alpha), LLFontGL::LEFT,
                     LLFontGL::VCENTER, llmax(0, S32(r.mRight - x)), true);
            drawFocusRingOf(focus, box, style::CHECK_RADIUS);
        }

        void radio(const LLRectf& r, bool on, bool hover, bool down, F32 alpha, const LLFontGL* font,
                   const LLWString& label, const LLUICtrl& focus)
        {
            const F32 s = F32(style::RADIO_SIZE);
            const F32 half = s * 0.5f;
            const F32 bottom = r.mBottom + std::floor((r.getHeight() - s) * 0.5f);
            const LLRectf mark(r.mLeft, bottom + s, r.mLeft + s, bottom);
            const F32 cx = mark.getCenterX();
            const F32 cy = mark.getCenterY();
            if (on)
            {
                const LLColor4 fill = down ? pressed(style::CHECK_ON) : hover ? hovered(style::CHECK_ON) : style::CHECK_ON;
                drawCircle(cx, cy, half, Fill::solid(faded(fill, alpha)));
                drawCircle(cx, cy, style::RADIO_DOT * 0.5f, Fill::solid(faded(style::CHECK_MARK_COLOR, alpha)));
            }
            else
            {
                drawRoundBordered(mark, half, Fill::solid(faded(style::CHECK_OFF, alpha)),
                                  faded(hover ? style::CHECK_BORDER_HOVER : style::CHECK_BORDER, alpha));
            }
            const F32 x = mark.mRight + F32(style::CHECK_GAP);
            drawText(font, label, x, r.mBottom + r.getHeight() * 0.5f, faded(style::CHECK_TEXT, alpha), LLFontGL::LEFT,
                     LLFontGL::VCENTER, llmax(0, S32(r.mRight - x)), true);
            drawFocusRingOf(focus, mark, half);
        }

        void sliderTrack(F32 x0, F32 x1, F32 kx, F32 mid, S32 ticks, bool dragging, bool hover, F32 alpha,
                         const LLUICtrl& focus)
        {
            const F32 t2 = style::SLIDER_TRACK * 0.5f;
            drawRound(LLRectf(x0, mid + t2, x1, mid - t2), t2, Fill::solid(faded(style::SLIDER_EMPTY, alpha)));
            if (kx > x0)
            {
                drawRound(LLRectf(x0, mid + t2, kx, mid - t2), t2,
                          Fill::horizontal(faded(style::SLIDER_FILL_LEFT, alpha), faded(style::SLIDER_FILL_RIGHT, alpha)),
                          CORNERS_LEFT);
            }
            if (ticks > 1)
            {
                for (S32 i = 0; i < ticks; ++i)
                {
                    const F32 tx = x0 + (x1 - x0) * F32(i) / F32(ticks - 1);
                    drawCircle(tx, mid + t2 + 5.f, 1.f, Fill::solid(faded(style::SLIDER_TICK, alpha)));
                }
            }
            const F32 kr = (dragging ? style::SLIDER_KNOB_PRESSED : style::SLIDER_KNOB) * 0.5f;
            drawShadow(kx, mid, kr, 0.35f * alpha);
            Fill knob = Fill::vertical(style::KNOB_TOP, style::KNOB_BOTTOM);
            if (hover && !dragging)
            {
                knob = knob.hovered();
            }
            drawCircle(kx, mid, kr, knob.faded(alpha));
            drawFocusRingOf(focus, LLRectf(kx - kr, mid + kr, kx + kr, mid - kr), kr);
        }

        void valueBox(const LLRectf& r, F32 alpha)
        {
            drawRound(r, style::VALUE_RADIUS, Fill::solid(faded(style::VALUE_FILL, alpha)));
        }

        void valueText(const LLRectf& r, const LLFontGL* font, const LLWString& text, F32 alpha)
        {
            drawText(font, text, r.mRight - F32(style::VALUE_PAD), r.getCenterY(), faded(style::TEXT_WHITE, alpha),
                     LLFontGL::RIGHT, LLFontGL::VCENTER, S32(r.getWidth()) - 2 * style::VALUE_PAD, true);
        }

        void button(const LLRectf& r, bool down, bool hover, bool on, F32 alpha, const LLFontGL* font,
                    const LLWString& label, const LLUIImage* icon, S32 icon_size, const LLUICtrl& focus)
        {
            if (on)
            {
                Fill fill = Fill::vertical(style::BUTTON_ON_TOP, style::BUTTON_ON_BOTTOM);
                fill = down ? fill.pressed() : hover ? fill.hovered() : fill;
                drawRoundBordered(r, style::BUTTON_RADIUS, fill.faded(alpha), faded(style::BUTTON_ON_BORDER, alpha));
            }
            else
            {
                drawButtonBox(r, down, hover, alpha);
            }
            // the words' room: the whole button but 6 at either side, or after the icon
            F32 left = r.mLeft + 6.f;
            F32 right = r.mRight - 6.f;
            if (icon)
            {
                const LLColor4 color = faded(style::ICON_LIGHT, alpha);
                if (label.empty())
                {
                    drawIcon(icon, r.getCenterX(), r.getCenterY(), icon_size, color);
                }
                else
                {
                    const F32 x = r.mLeft + F32(style::BUTTON_PAD);
                    drawIcon(icon, x + F32(icon_size) * 0.5f, r.getCenterY(), icon_size, color);
                    left = x + F32(icon_size + style::CHIP_GAP);
                    right = r.mRight - F32(style::BUTTON_PAD);
                }
            }
            drawText(font, label, (left + right) * 0.5f, r.getCenterY(), faded(style::TEXT_WHITE, alpha), LLFontGL::HCENTER,
                     LLFontGL::VCENTER, llmax(0, S32(right - left)), true);
            drawFocusRingOf(focus, r, style::BUTTON_RADIUS);
        }

        F32 segmentEdge(const LLRectf& r, S32 index, S32 count)
        {
            return r.mLeft + std::floor(r.getWidth() * F32(index) / F32(llmax(1, count)));
        }

        void segmentBar(const LLRectf& r, S32 count, S32 chosen, F32 alpha)
        {
            drawRoundBordered(r, style::SEG_RADIUS, Fill::solid(faded(style::SEG_OFF, alpha)),
                              faded(style::SEG_BORDER_DIM, alpha));
            // a thin line between two choices that are not chosen (not in the picture, in its style: three or more)
            for (S32 i = 1; i < count; ++i)
            {
                if (i != chosen && i - 1 != chosen)
                {
                    const F32 x = segmentEdge(r, i, count);
                    fillRect(LLRectf(x, r.mTop - style::SEG_SEPARATOR_INSET, x + 1.f, r.mBottom + style::SEG_SEPARATOR_INSET),
                             Fill::solid(faded(style::SEG_BORDER_DIM, alpha)));
                }
            }
            if (chosen >= 0 && chosen < count)
            {
                const LLRectf on(segmentEdge(r, chosen, count), r.mTop,
                                 chosen + 1 < count ? segmentEdge(r, chosen + 1, count) : r.mRight, r.mBottom);
                const U8 corners = static_cast<U8>((chosen == 0 ? CORNERS_LEFT : CORNERS_NONE) |
                                                   (chosen == count - 1 ? CORNERS_RIGHT : CORNERS_NONE));
                drawRoundBordered(on, style::SEG_RADIUS,
                                  Fill::horizontal(faded(style::SEG_ON_LEFT, alpha), faded(style::SEG_ON_RIGHT, alpha)),
                                  faded(style::SEG_BORDER, alpha), 1.f, corners);
            }
        }

        void segmentLabel(const LLRectf& r, S32 index, S32 count, bool chosen, bool hovered, const LLFontGL* font,
                          const LLWString& label, F32 alpha)
        {
            const F32 x0 = segmentEdge(r, index, count);
            const F32 x1 = index + 1 < count ? segmentEdge(r, index + 1, count) : r.mRight;
            const LLColor4& color = chosen ? style::TEXT_WHITE : hovered ? style::TEXT_LABEL : style::TEXT_DIM;
            drawText(font, label, (x0 + x1) * 0.5f, r.getCenterY(), faded(color, alpha), LLFontGL::HCENTER,
                     LLFontGL::VCENTER, S32(x1 - x0) - 4, true);
        }

        void searchHit(const LLRectf& r, F32 context_alpha)
        {
            drawRound(r, style::SEARCH_HIT_RADIUS, Fill::solid(faded(style::SEARCH_HIT_FILL, context_alpha)));
            drawRing(r, style::SEARCH_HIT_RADIUS, 1.f, faded(style::SEARCH_HIT_RING, context_alpha));
        }
    }

    // =========================================================================================
    // PsButtonCompat

    PsButtonCompat::Params::Params()
    :   icon("icon"),
        icon_size("icon_size", style::BUTTON_ICON)
    {
    }

    PsButtonCompat::PsButtonCompat(const Params& p)
    :   LLButton(p),
        mFont(font(style::FONT_BUTTON)),
        mIcon(psui::icon(p.icon())),
        mIconSize(llmax(1, p.icon_size()))
    {
    }

    bool PsButtonCompat::pressedNow()
    {
        // as the official button: held down with the mouse over it
        return hasMouseCapture() && mouseIn(*this, getLocalRect());
    }

    void PsButtonCompat::draw()
    {
        const LLRectf r = localRectOf(*this);
        if (getHighlighted())
        {
            look::searchHit(r, getDrawContext().mAlpha);
        }
        bool on = getToggleState();
        if (getFlashing())
        {
            const LLFlashTimer* flash = getFlashTimer();
            on = on || (flash && flash->isCurrentlyHighlighted());
        }
        // a psui icon, or the official overlay picture at its own size (within the button)
        const LLUIImage* image = mIcon.get();
        S32 size = mIconSize;
        if (!image && mImageOverlay.notNull())
        {
            image = mImageOverlay.get();
            size = llmin(llmax(image->getWidth(), image->getHeight()), getRect().getHeight() - 8);
        }
        look::button(r, pressedNow(), mNeedsHighlight && getEnabled(), on, drawAlphaOf(*this), mFont,
                     getCurrentLabel().getWString(), image, size, *this);
        LLUICtrl::draw();   // children (a badge)
    }

    // =========================================================================================
    // PsCheckCompat

    PsCheckCompat::Params::Params()
    {
        tab_stop(true);
    }

    PsCheckCompat::PsCheckCompat(const Params& p)
    :   LLCheckBoxCtrl(p),
        mFont(font(style::FONT_BODY)),
        mMark(icon("check"))
    {
        placeChildren();
    }

    void PsCheckCompat::placeChildren()
    {
        const S32 w = getRect().getWidth();
        const S32 h = getRect().getHeight();
        if (mLabel)
        {
            mLabel->setShape(LLRect(llmin(w, style::CHECK_BOX + style::CHECK_GAP), h, w, 0));
        }
        if (mButton)
        {
            mButton->setShape(LLRect(0, h, w, 0));
        }
        updateBoundingRect();
    }

    void PsCheckCompat::reshape(S32 width, S32 height, bool called_from_parent)
    {
        LLCheckBoxCtrl::reshape(width, height, called_from_parent);
        placeChildren();
    }

    void PsCheckCompat::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLCheckBoxCtrl::onMouseEnter(x, y, mask);
    }

    void PsCheckCompat::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        LLCheckBoxCtrl::onMouseLeave(x, y, mask);
    }

    void PsCheckCompat::draw()
    {
        const LLRectf r = localRectOf(*this);
        if (getHighlighted() || highlighted(mLabel))
        {
            look::searchHit(r, getDrawContext().mAlpha);
        }
        const bool on = mButton && mButton->getToggleState();
        const bool down = mButton && mButton->hasMouseCapture();
        // the focus is the button's (hasFocus(): this one or a child)
        look::check(r, on, mHover && getEnabled(), down, drawAlphaOf(*this), mFont, wtextOf(mLabel), mMark.get(), *this);
    }

    // =========================================================================================
    // PsSliderCompat

    PsSliderCompat::Params::Params()
    :   ticks("ticks", 0)
    {
        label_width(style::SLIDER_LABEL_WIDTH);
        text_width(style::VALUE_WIDTH);
        show_text(true);
        can_edit_text(true);
        tab_stop(true);
    }

    PsSliderCompat::PsSliderCompat(const Params& p)
    :   LLSliderCtrl(p),
        mThumb(p.slider_bar.thumb_image()),
        mFont(font(style::FONT_BODY)),
        mLabelWidth(llmax(0, p.label_width())),
        mValueWidth(llmax(0, p.text_width())),
        mTicks(llclamp(p.ticks(), 0, 64))
    {
        // The official children (LLSliderCtrl keeps them private): the bar and the editor are the only ones of their
        // class; the label and the value's text are found by the names of their parameters.
        for (LLView* child : *getChildList())
        {
            if (LLSlider* bar = dynamic_cast<LLSlider*>(child))
            {
                mBar = bar;
            }
            else if (LLLineEditor* editor = dynamic_cast<LLLineEditor*>(child))
            {
                mEditor = editor;
            }
        }
        if (!p.label().empty())
        {
            mLabelText = findChild<LLTextBox>(p.slider_label.name(), false);
        }
        if (p.show_text() && !p.can_edit_text())
        {
            mValueText = findChild<LLTextBox>(p.value_text.name(), false);
        }
        styleValueEditor(mEditor);
        placeChildren();
    }

    LLRectf PsSliderCompat::valueRect() const
    {
        const F32 w = F32(getRect().getWidth());
        const F32 mid = std::floor(F32(getRect().getHeight()) * 0.5f);
        const F32 vh = F32(style::VALUE_HEIGHT) * 0.5f;
        const F32 vw = (mEditor || mValueText) ? F32(mValueWidth) : 0.f;
        return LLRectf(w - vw, mid + vh, w, mid - vh);
    }

    void PsSliderCompat::placeChildren()
    {
        const S32 w = getRect().getWidth();
        const S32 h = getRect().getHeight();
        const S32 label_w = mLabelText ? llmin(mLabelWidth, w) : 0;
        const LLRectf v = valueRect();
        const S32 bar_right = llmax(label_w, v.getWidth() > 0.f ? S32(v.mLeft) - style::SLIDER_GAP : w);
        if (mLabelText)
        {
            mLabelText->setShape(LLRect(0, h, label_w, 0));
        }
        if (mBar)
        {
            mBar->setRect(LLRect(label_w, h, bar_right, 0));
        }
        if (mEditor)
        {
            mEditor->setRect(toRect(v));
        }
        if (mValueText)
        {
            mValueText->setShape(toRect(v));
        }
    }

    void PsSliderCompat::reshape(S32 width, S32 height, bool called_from_parent)
    {
        LLSliderCtrl::reshape(width, height, called_from_parent);
        placeChildren();
    }

    void PsSliderCompat::setRect(const LLRect& rect)
    {
        LLSliderCtrl::setRect(rect);
        placeChildren();
    }

    bool PsSliderCompat::handleKeyHere(KEY key, MASK mask)
    {
        if (mBar && getEnabled() && mask == MASK_NONE && (key == KEY_HOME || key == KEY_END))
        {
            // as the bar's arrows (LLSlider::setValueAndCommit): the control first, then the commit
            const F32 v = key == KEY_HOME ? getMinValue() : getMaxValue();
            if (v != mBar->getValueF32())
            {
                mBar->setValue(v);
                mBar->onCommit();
            }
            return true;
        }
        return LLSliderCtrl::handleKeyHere(key, mask);
    }

    void PsSliderCompat::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLSliderCtrl::onMouseEnter(x, y, mask);
    }

    void PsSliderCompat::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        LLSliderCtrl::onMouseLeave(x, y, mask);
    }

    void PsSliderCompat::draw()
    {
        const LLRectf r = localRectOf(*this);
        const F32 a = drawAlphaOf(*this);
        const F32 mid = std::floor(r.getHeight() * 0.5f);
        if (getHighlighted() || highlighted(mLabelText))
        {
            look::searchHit(r, getDrawContext().mAlpha);
        }
        if (mLabelText)
        {
            drawText(mFont, mLabelText->getWText(), 0.f, mid, faded(style::LABEL, a), LLFontGL::LEFT, LLFontGL::VCENTER,
                     mLabelText->getRect().getWidth() - 6, true);
        }
        if (mBar)
        {
            // where the official bar puts the value under the mouse: its thumb's half width in from either end
            const LLRect& bar = mBar->getRect();
            const S32 half = mThumb.notNull() ? mThumb->getWidth() / 2 : S32(style::SLIDER_KNOB * 0.5f);
            const F32 x0 = F32(bar.mLeft + half);
            const F32 x1 = llmax(x0, F32(bar.mRight - half));
            const F32 range = getMaxValue() - getMinValue();
            const F32 t = range > 0.f ? llclamp((mBar->getValueF32() - getMinValue()) / range, 0.f, 1.f) : 0.f;
            look::sliderTrack(x0, x1, x0 + (x1 - x0) * t, mid, mTicks, mBar->hasMouseCapture(), mHover && getEnabled(), a,
                              *mBar);
        }
        if (mEditor || mValueText)
        {
            const LLRectf v = valueRect();
            look::valueBox(v, a);
            if (mEditor && mEditor->hasFocus())
            {
                drawChild(mEditor);     // typing
            }
            else
            {
                look::valueText(v, mFont, mEditor ? mEditor->getWText() : mValueText->getWText(), a);
            }
        }
    }

    // =========================================================================================
    // PsSpinner

    PsSpinner::Params::Params()
    {
        label_width(style::SLIDER_LABEL_WIDTH);
        tab_stop(true);
    }

    PsSpinner::PsSpinner(const Params& p)
    :   LLSpinCtrl(p),
        mFont(font(style::FONT_BODY)),
        mChevronUp(icon("chevron_up")),
        mChevronDown(icon("chevron_down")),
        mLabelWidth(llmax(0, p.label_width()))
    {
        // the official children's names (llspinctrl.cpp; the buttons' from their parameters)
        mLabelText = findChild<LLTextBox>("SpinCtrl Label", false);
        mEditor = findChild<LLLineEditor>("SpinCtrl Editor", false);
        mUp = findChild<LLButton>(p.up_button.name(), false);
        mDown = findChild<LLButton>(p.down_button.name(), false);
        styleValueEditor(mEditor);
        placeChildren();
    }

    LLRectf PsSpinner::valueRect() const
    {
        const S32 w = getRect().getWidth();
        // the official label's room (LLSpinCtrl leaves 40 for the value)
        const S32 label_w = mLabelText ? llclamp(mLabelWidth, 0, llmax(0, w - 40)) : 0;
        const F32 mid = std::floor(F32(getRect().getHeight()) * 0.5f);
        const F32 vh = F32(style::VALUE_HEIGHT) * 0.5f;
        return LLRectf(F32(label_w), mid + vh, F32(w), mid - vh);
    }

    void PsSpinner::placeChildren()
    {
        const S32 h = getRect().getHeight();
        const LLRect v = toRect(valueRect());
        const S32 arrows = v.mRight - llmin(style::SPIN_ARROW_WIDTH, v.getWidth());
        const S32 mid = (v.mTop + v.mBottom) / 2;
        if (mLabelText)
        {
            mLabelText->setShape(LLRect(0, h, v.mLeft, 0));
        }
        if (mEditor)
        {
            mEditor->setRect(LLRect(v.mLeft, v.mTop, arrows, v.mBottom));
        }
        if (mUp)
        {
            mUp->setShape(LLRect(arrows, v.mTop, v.mRight, mid));
        }
        if (mDown)
        {
            mDown->setShape(LLRect(arrows, mid, v.mRight, v.mBottom));
        }
    }

    void PsSpinner::reshape(S32 width, S32 height, bool called_from_parent)
    {
        LLSpinCtrl::reshape(width, height, called_from_parent);
        placeChildren();
    }

    bool PsSpinner::handleScrollWheel(S32 x, S32 y, S32 clicks)
    {
        // the value changes with the wheel only while it is typed in; else the panel under it scrolls
        if (mEditor && mEditor->hasFocus())
        {
            return LLSpinCtrl::handleScrollWheel(x, y, clicks);
        }
        return false;
    }

    void PsSpinner::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLSpinCtrl::onMouseEnter(x, y, mask);
    }

    void PsSpinner::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        LLSpinCtrl::onMouseLeave(x, y, mask);
    }

    void PsSpinner::draw()
    {
        const F32 a = drawAlphaOf(*this);
        const LLRectf v = valueRect();
        if (highlighted(mLabelText))       // LLSpinCtrl is not searchable itself: its label is
        {
            look::searchHit(localRectOf(*this), getDrawContext().mAlpha);
        }
        if (mLabelText)
        {
            drawText(mFont, mLabelText->getWText(), 0.f, v.getCenterY(), faded(style::LABEL, a), LLFontGL::LEFT,
                     LLFontGL::VCENTER, S32(v.mLeft) - 6, true);
        }
        look::valueBox(v, a);
        const bool typing = mEditor && mEditor->hasFocus();
        if (typing)
        {
            drawChild(mEditor);
        }
        else if (mEditor)
        {
            // where the editor is (left of the arrows)
            look::valueText(LLRectf(v.mLeft, v.mTop, F32(mEditor->getRect().mRight), v.mBottom), mFont,
                            mEditor->getWText(), a);
        }
        for (LLButton* arrow : { mUp, mDown })
        {
            if (!arrow)
            {
                continue;
            }
            const LLRect& ar = arrow->getRect();
            const bool down = arrow->hasMouseCapture();
            const bool over = mHover && getEnabled() && mouseIn(*this, ar);
            const LLColor4& color = down ? style::ACCENT_LINE : over ? style::TEXT_WHITE : style::TEXT_LABEL;
            drawIcon((arrow == mUp ? mChevronUp : mChevronDown).get(), F32(ar.getCenterX()), F32(ar.getCenterY()),
                     style::SPIN_ARROW, faded(color, a));
        }
        if (mEditor)
        {
            drawFocusRingOf(*mEditor, v, style::VALUE_RADIUS);
        }
    }

    // =========================================================================================
    // PsText

    PsText::Params::Params()
    :   kind("kind", "label")
    {
        font_valign(LLFontGL::VCENTER);     // one line in the middle of its row, as plus_ps_label
    }

    PsText::PsText(const Params& p)
    :   LLTextBox(p)
    {
        const std::string& k = p.kind;
        LLColor4 color = style::LABEL;
        const LLFontGL* f = font(style::FONT_BODY);
        if (k == "white")       { color = style::TEXT_WHITE; }
        else if (k == "dim")    { color = style::TEXT_DIM; }
        else if (k == "sub")    { color = style::TEXT_SUB;   f = font(style::FONT_SMALL); }
        else if (k == "hint")   { color = style::TEXT_HINT;  f = font(style::FONT_SMALL); }
        else if (k == "title")  { color = style::TEXT_WHITE; f = font(style::FONT_BUTTON); }
        setColor(LLUIColor(color));
        setReadOnlyColor(LLUIColor(color));     // disabled: the same colour at 40 % (draw())
        setFont(f);                             // before the text: initFromParams() sets it after this
    }

    void PsText::draw()
    {
        if (ll::ui::SearchableControl::getHighlighted())
        {
            look::searchHit(localRectOf(*this), getDrawContext().mAlpha);
        }
        const HighlightOff no_official_highlight(*this);
        const LLViewDrawContext fade(getEnabled() ? 1.f : style::STATE_DISABLED);
        LLTextBox::draw();
    }

    // =========================================================================================
    // PsRadio

    PsRadio::Params::Params()
    :   kind("kind", "list")
    {
        mouse_opaque(true);     // the clicks on an item are taken here (radio_group.xml: false)
    }

    PsRadio::PsRadio(const Params& p)
    :   LLRadioGroup(p),
        mSegment(p.kind() == "segment"),
        mFont(font(style::FONT_BODY)),
        mSegmentFont(font(style::FONT_BUTTON))
    {
    }

    bool PsRadio::postBuild()
    {
        // The items are the group's children, added to the front of the list one by one (LLView::addChild): from the
        // back, they are in the order of the XML, which is the group's order (getSelectedIndex()).
        mItems.clear();
        const child_list_t* children = getChildList();
        for (auto it = children->rbegin(); it != children->rend(); ++it)
        {
            if (LLCheckBoxCtrl* item = dynamic_cast<LLCheckBoxCtrl*>(*it))
            {
                mItems.push_back(item);
            }
        }
        placeItems();
        return LLRadioGroup::postBuild();
    }

    void PsRadio::placeItems()
    {
        if (!mSegment)
        {
            return;     // a list keeps the places of the XML
        }
        const LLRectf r = localRectOf(*this);
        const S32 count = (S32)mItems.size();
        for (S32 i = 0; i < count; ++i)
        {
            const F32 x0 = look::segmentEdge(r, i, count);
            const F32 x1 = i + 1 < count ? look::segmentEdge(r, i + 1, count) : r.mRight;
            mItems[i]->setShape(LLRect(S32(x0), S32(r.mTop), S32(x1), 0));
        }
    }

    void PsRadio::reshape(S32 width, S32 height, bool called_from_parent)
    {
        LLRadioGroup::reshape(width, height, called_from_parent);
        placeItems();
    }

    S32 PsRadio::itemAt(S32 x, S32 y) const
    {
        const S32 count = (S32)mItems.size();
        if (mSegment)
        {
            if (!pointInView(x, y))
            {
                return -1;
            }
            const LLRectf r = localRectOf(*this);
            for (S32 i = 0; i + 1 < count; ++i)
            {
                if (F32(x) < look::segmentEdge(r, i + 1, count))
                {
                    return i;
                }
            }
            return count - 1;
        }
        for (S32 i = 0; i < count; ++i)
        {
            const LLCheckBoxCtrl* item = mItems[i];
            if (item->getVisible() && item->getRect().pointInRect(x, y))
            {
                return i;
            }
        }
        return -1;
    }

    bool PsRadio::itemHighlighted(LLCheckBoxCtrl* item) const
    {
        return item->getHighlighted() || highlighted(item->getTextBox());
    }

    bool PsRadio::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        const S32 i = itemAt(x, y);
        if (i < 0)
        {
            return LLRadioGroup::handleMouseDown(x, y, mask);
        }
        if (getEnabled() && mItems[i]->getEnabled())
        {
            mPressedItem = i;
            gFocusMgr.setMouseCapture(this);
            focusSelectedRadioBtn();    // as an official item does (LLRadioCtrl::handleMouseDown)
        }
        return true;
    }

    bool PsRadio::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (!hasMouseCapture())
        {
            return LLRadioGroup::handleMouseUp(x, y, mask);
        }
        gFocusMgr.setMouseCapture(nullptr);
        const S32 i = mPressedItem;
        mPressedItem = -1;
        if (i >= 0 && i == itemAt(x, y) && getEnabled() && mItems[i]->getEnabled())
        {
            onClickButton(mItems[i]);      // the official item's click: chooses it and commits (last: it may close us)
        }
        return true;
    }

    bool PsRadio::handleHover(S32 x, S32 y, MASK mask)
    {
        mHoverItem = itemAt(x, y);
        LLRadioGroup::handleHover(x, y, mask);
        return true;
    }

    bool PsRadio::handleKeyHere(KEY key, MASK mask)
    {
        if (getEnabled() && mask == MASK_NONE && (key == KEY_HOME || key == KEY_END) && !mItems.empty())
        {
            const S32 i = key == KEY_HOME ? 0 : (S32)mItems.size() - 1;
            if (i != getSelectedIndex() && setSelectedIndex(i))
            {
                onCommit();
            }
            return true;
        }
        return LLRadioGroup::handleKeyHere(key, mask);
    }

    void PsRadio::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLRadioGroup::onMouseEnter(x, y, mask);
    }

    void PsRadio::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        mHoverItem = -1;
        LLRadioGroup::onMouseLeave(x, y, mask);
    }

    void PsRadio::onMouseCaptureLost()
    {
        mPressedItem = -1;
        LLRadioGroup::onMouseCaptureLost();
    }

    void PsRadio::draw()
    {
        const F32 a = drawAlphaOf(*this);
        const F32 context_alpha = getDrawContext().mAlpha;
        const bool hover = mHover && getEnabled();
        const S32 count = (S32)mItems.size();
        const S32 chosen = getSelectedIndex();
        if (mSegment)
        {
            const LLRectf r = localRectOf(*this);
            bool hit = false;
            for (LLCheckBoxCtrl* item : mItems)
            {
                hit = hit || itemHighlighted(item);
            }
            if (hit)
            {
                look::searchHit(r, context_alpha);
            }
            look::segmentBar(r, count, chosen, a);
            for (S32 i = 0; i < count; ++i)
            {
                const F32 ia = mItems[i]->getEnabled() ? a : a * style::STATE_DISABLED;
                look::segmentLabel(r, i, count, i == chosen, hover && i == mHoverItem, mSegmentFont,
                                   wtextOf(mItems[i]->getTextBox()), ia);
            }
            drawFocusRingOf(*this, r, style::SEG_RADIUS);     // an item (its button) has the focus
            return;
        }
        for (S32 i = 0; i < count; ++i)
        {
            LLCheckBoxCtrl* item = mItems[i];
            if (!item->getVisible())
            {
                continue;
            }
            const LLRect& ir = item->getRect();
            const LLRectf rect(F32(ir.mLeft), F32(ir.mTop), F32(ir.mRight), F32(ir.mBottom));
            if (itemHighlighted(item))
            {
                look::searchHit(rect, context_alpha);
            }
            const bool enabled = item->getEnabled();
            look::radio(rect, i == chosen, hover && enabled && i == mHoverItem, i == mPressedItem,
                        enabled ? a : a * style::STATE_DISABLED, mFont, wtextOf(item->getTextBox()), *item);
        }
    }

    // =========================================================================================

    bool isCompatWidget(const LLView* view)
    {
        return dynamic_cast<const PsButtonCompat*>(view) || dynamic_cast<const PsCheckCompat*>(view) ||
               dynamic_cast<const PsSliderCompat*>(view) || dynamic_cast<const PsSpinner*>(view) ||
               dynamic_cast<const PsText*>(view) || dynamic_cast<const PsRadio*>(view);
    }
}
