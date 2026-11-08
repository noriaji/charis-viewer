/**
 * @file pswidgets.cpp
 * @brief The photo screen's widgets: boxes, rail, headings, labels and controls (research/221).
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

// The cards, pictures, the world's overlay and the shutter strip are in pscards.cpp.

#include "llviewerprecompiledheaders.h"

#include "pswidgets.h"
#include "pscompat.h"
#include "psstyle.h"

#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "lluictrlfactory.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <typeinfo>
#include <vector>

namespace plus::psui
{
    namespace
    {
        LLDefaultChildRegistry::Register<PsBox>     sBoxRegistration("plus_ps_box");
        LLDefaultChildRegistry::Register<PsRow>     sRowRegistration("plus_ps_row");
        LLDefaultChildRegistry::Register<PsRailTab> sRailTabRegistration("plus_ps_rail_tab");
        LLDefaultChildRegistry::Register<PsHeader>  sHeaderRegistration("plus_ps_header");
        LLDefaultChildRegistry::Register<PsLabel>   sLabelRegistration("plus_ps_label");
        LLDefaultChildRegistry::Register<PsDivider> sDividerRegistration("plus_ps_divider");
        LLDefaultChildRegistry::Register<PsCheck>   sCheckRegistration("plus_ps_check");
        LLDefaultChildRegistry::Register<PsSwitch>  sSwitchRegistration("plus_ps_switch");
        LLDefaultChildRegistry::Register<PsSlider>  sSliderRegistration("plus_ps_slider");
        LLDefaultChildRegistry::Register<PsCombo>   sComboRegistration("plus_ps_combo");

        LLFontGL::HAlign parseAlign(const std::string& text)
        {
            if (text == "center") return LLFontGL::HCENTER;
            if (text == "right")  return LLFontGL::RIGHT;
            return LLFontGL::LEFT;
        }

        F32 lineHeight(const LLFontGL* f)
        {
            return f ? F32(f->getLineHeight()) : 14.f;
        }
    }

    // =========================================================================================
    // PsCtrl

    PsCtrl::Params::Params()
    :   icon("icon")    // named: the XUI attribute icon="..." (an unnamed param is never read from the XML)
    {
        mouse_opaque(true);
    }

    PsCtrl::PsCtrl(const Params& p)
    :   LLUICtrl(p),
        mIcon(icon(p.icon)),
        mLabel(p.label),
        mLabelW(utf8str_to_wstring(p.label()))
    {
    }

    void PsCtrl::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLUICtrl::onMouseEnter(x, y, mask);
    }

    void PsCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        LLUICtrl::onMouseLeave(x, y, mask);
    }

    bool PsCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        if (getEnabled())
        {
            mPressed = true;
            gFocusMgr.setMouseCapture(this);
        }
        return true;
    }

    bool PsCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
            const bool inside = pointInView(x, y);
            mPressed = false;
            if (inside && getEnabled())
            {
                onClick(x, y);
            }
        }
        return true;
    }

    bool PsCtrl::handleKeyHere(KEY key, MASK mask)
    {
        if (getEnabled() && mask == MASK_NONE && ((key == KEY_RETURN && clicksOnReturn()) || key == ' '))
        {
            // one press, one click (as LLButton): a held key's repeats are taken but do nothing
            if (!(gKeyboard && gKeyboard->getKeyRepeated(key)))
            {
                onClick(-1, -1);
            }
            return true;
        }
        return LLUICtrl::handleKeyHere(key, mask);
    }

    void PsCtrl::onMouseCaptureLost()
    {
        mPressed = false;
    }

    void PsCtrl::setLabelText(const std::string& label)
    {
        if (label == mLabel)
        {
            return;
        }
        mLabel = label;
        mLabelW = utf8str_to_wstring(label);
        onLabelChanged();
    }

    void PsCtrl::setIcon(const std::string& name)
    {
        mIcon = icon(name);
    }

    void PsCtrl::clearSiblingValues()
    {
        LLView* parent = getParent();
        if (!parent)
        {
            return;
        }
        for (LLView* child : *parent->getChildList())
        {
            if (isSibling(child))
            {
                LLUICtrl* other = static_cast<LLUICtrl*>(child);
                if (other->getValue().asBoolean())
                {
                    other->setValue(false);
                }
            }
        }
    }

    void PsCtrl::takeFocusFromSibling()
    {
        if (isSibling(dynamic_cast<const LLView*>(gFocusMgr.getKeyboardFocus())))
        {
            setFocus(true);
        }
    }

    bool PsCtrl::isSibling(const LLView* view) const
    {
        return view && view != this && view->getParent() == getParent() && typeid(*view) == typeid(*this);
    }

    void PsCtrl::commitChange()
    {
        setControlValue(getValue());
        onCommit();
    }

    F32 PsCtrl::drawAlpha() const
    {
        return drawAlphaOf(*this);
    }

    void PsCtrl::drawFocusRing(const LLRectf& r, F32 radius) const
    {
        drawFocusRingOf(*this, r, radius);
    }

    void PsCtrl::drawSearchHighlight(const LLRectf& r) const
    {
        if (getHighlighted())
        {
            look::searchHit(r, getDrawContext().mAlpha);
        }
    }

    LLRectf PsCtrl::localRectf() const
    {
        return localRectOf(*this);
    }

    void releaseFocusIn(const LLView* hidden, LLUICtrl* to)
    {
        if (!hidden || !gFocusMgr.childHasKeyboardFocus(hidden))
        {
            return;
        }
        if (to)
        {
            to->setFocus(true);
        }
        if (gFocusMgr.childHasKeyboardFocus(hidden))
        {
            gFocusMgr.setKeyboardFocus(nullptr);
        }
    }

    // =========================================================================================
    // PsBox

    PsBox::Params::Params()
    :   kind("kind", "panel"),
        corners("corners", "all")
    {
        background_visible(false);
    }

    PsBox::PsBox(const Params& p)
    :   LLPanel(p),
        mKind(Kind::PANEL),
        mCorners(parseCorners(p.corners))
    {
        const std::string& k = p.kind;
        if (k == "rail")                mKind = Kind::RAIL;
        else if (k == "ground")         mKind = Kind::GROUND;
        else if (k == "glass")          mKind = Kind::GLASS;
        else if (k == "world_corners")  mKind = Kind::WORLD_CORNERS;
    }

    void PsBox::draw()
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        const LLRectf r(0.f, h, w, 0.f);
        const F32 a = getDrawContext().mAlpha;
        switch (mKind)
        {
        case Kind::PANEL:
            drawRoundBordered(r, style::PANEL_RADIUS, Fill::vertical(style::PANEL_TOP, style::PANEL_BOTTOM).faded(a),
                              faded(style::PANEL_BORDER, a), 1.f, mCorners);
            break;
        case Kind::RAIL:
            drawRound(r, style::PANEL_RADIUS, Fill::solid(faded(style::RAIL, a)), mCorners);
            fillRect(LLRectf(w - 1.f, h, w, 0.f), Fill::solid(faded(style::PANEL_BORDER, a)));
            break;
        case Kind::GROUND:
            fillRect(r, Fill::solid(faded(style::GROUND, a)));
            break;
        case Kind::GLASS:
            drawRound(r, style::CHIP_RADIUS, Fill::solid(faded(style::CHIP_FILL, a)), mCorners);
            break;
        case Kind::WORLD_CORNERS:
            break;      // drawn over the children below
        }
        LLPanel::draw();
        if (mKind == Kind::WORLD_CORNERS)
        {
            drawCornerCuts(r, style::WORLD_RADIUS, faded(style::GROUND, a), mCorners);
        }
    }

    // =========================================================================================
    // PsRow

    PsRow::Params::Params()
    :   kind("kind", "none"),
        spacing("spacing", style::CHIP_GAP),
        pad("pad", 0)
    {
        background_visible(false);
        mouse_opaque(false);
    }

    PsRow::PsRow(const Params& p)
    :   LLPanel(p),
        mTabs(p.kind() == "tabs"),
        mSpacing(p.spacing),
        mPad(p.pad)
    {
    }

    bool PsRow::postBuild()
    {
        arrange();
        return LLPanel::postBuild();
    }

    void PsRow::arrange()
    {
        // Not while drawing: when built and when a child's width changed.
        std::vector<LLView*> views;
        for (LLView* child : *getChildList())
        {
            if (child->getVisible())
            {
                views.push_back(child);
            }
        }
        std::stable_sort(views.begin(), views.end(),
                         [](const LLView* a, const LLView* b) { return a->getRect().mLeft < b->getRect().mLeft; });
        S32 x = mPad;
        for (LLView* v : views)
        {
            LLRect rect = v->getRect();
            if (rect.mLeft != x)
            {
                rect.translate(x - rect.mLeft, 0);
                v->setRect(rect);
            }
            x += rect.getWidth() + mSpacing;
        }
    }

    void PsRow::draw()
    {
        if (mTabs)
        {
            const F32 w = F32(getRect().getWidth());
            fillRect(LLRectf(0.f, 1.f, w, 0.f), Fill::solid(faded(style::DIVIDER_LEFT, getDrawContext().mAlpha)));
        }
        LLPanel::draw();
    }

    // =========================================================================================
    // PsRailTab

    PsRailTab::Params::Params()
    {
        tab_stop(true);
    }

    PsRailTab::PsRailTab(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_BODY))
    {
        onLabelChanged();
    }

    void PsRailTab::onLabelChanged()
    {
        // Up to two lines, split where the label has a line break ("Pose &&#10;Animation").
        const size_t cut = mLabelW.find(L'\n');
        mLines[0] = mLabelW.substr(0, cut);
        mLines[1] = cut == LLWString::npos ? LLWString() : mLabelW.substr(cut + 1);
    }

    void PsRailTab::setValue(const LLSD& value)
    {
        mSelected = value.asBoolean();
    }

    void PsRailTab::setBadge(S32 count)
    {
        count = llmax(0, count);
        if (count != mBadge)
        {
            mBadge = count;
            mBadgeText = badgeText(count);
        }
    }

    void PsRailTab::onClick(S32, S32)
    {
        takeFocusFromSibling();
        if (!mSelected)
        {
            mSelected = true;
            clearSiblingValues();
            commitChange();
        }
    }

    void PsRailTab::draw()
    {
        const LLRectf r = localRectf();
        const F32 w = r.getWidth();
        const F32 h = r.getHeight();
        const F32 a = drawAlpha();
        if (mSelected)
        {
            fillRect(r, Fill::horizontal(style::RAIL_SEL_LEFT, style::RAIL_SEL_RIGHT).faded(a));
            drawRound(LLRectf(0.f, h, style::RAIL_BAR_WIDTH, 0.f), style::RAIL_BAR_WIDTH * 0.5f,
                      Fill::solid(faded(style::RAIL_SEL_BAR, a)));
        }
        else if (mPressed)
        {
            fillRect(r, Fill::solid(faded(style::RAIL_PRESS, a)));
        }
        else if (mHover && getEnabled())
        {
            fillRect(r, Fill::solid(faded(style::RAIL_HOVER, a)));
        }

        const LLColor4 icon_color = faded(mSelected ? style::RAIL_SEL_TEXT : style::RAIL_ICON_COLOR, a);
        const LLColor4 text_color = faded(mSelected ? style::RAIL_SEL_TEXT : style::RAIL_TEXT, a);
        drawIcon(mIcon.get(), w * 0.5f, h - F32(style::RAIL_ICON_TOP) - F32(style::RAIL_ICON) * 0.5f, style::RAIL_ICON, icon_color);
        F32 y = h - F32(style::RAIL_TEXT_TOP);
        for (const LLWString& line : mLines)
        {
            drawText(mFont, line, w * 0.5f, y, text_color, LLFontGL::HCENTER, LLFontGL::TOP, S32(w) - 8, true);
            y -= lineHeight(mFont);
        }
        if (mBadge > 0)
        {
            // on the icon's top right corner
            const F32 icon_half = F32(style::RAIL_ICON) * 0.5f;
            drawBadge(mBadgeText, w * 0.5f + icon_half, h - F32(style::RAIL_ICON_TOP), false, a);
        }
        drawSearchHighlight(r);
        drawFocusRing(LLRectf(2.f, h - 2.f, w - 2.f, 2.f), 2.f);
    }

    // =========================================================================================
    // PsHeader

    PsHeader::Params::Params()
    :   size("size", "panel"),
        divider("divider", "none"),
        collapsible("collapsible", true)
    {
        tab_stop(true);
    }

    PsHeader::PsHeader(const Params& p)
    :   PsCtrl(p),
        mSize(p.size() == "right" ? Size::RIGHT : p.size() == "sub" ? Size::SUB : Size::PANEL),
        mDivider(p.divider() == "top" ? 1 : p.divider() == "bottom" ? -1 : 0),
        mCollapsible(p.collapsible.isProvided() ? p.collapsible() : p.size() != "sub"),
        mFont(font(p.size() == "panel" || !p.size.isProvided() ? style::FONT_TITLE : style::FONT_HEAD)),
        mChevronUp(icon("chevron_up")),
        mChevronDown(icon("chevron_down"))
    {
        if (p.initial_value.isProvided())
        {
            mOpen = p.initial_value().asBoolean();
        }
        if (!mCollapsible)
        {
            setTabStop(false);
        }
    }

    void PsHeader::setValue(const LLSD& value)
    {
        mOpen = value.asBoolean();
    }

    void PsHeader::onClick(S32, S32)
    {
        if (mCollapsible)
        {
            mOpen = !mOpen;
            commitChange();
        }
    }

    void PsHeader::draw()
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        const F32 a = drawAlpha();
        const F32 pad = mSize == Size::SUB ? 0.f : F32(style::HEAD_PAD);

        if (mDivider != 0)
        {
            const LLColor4 line = faded(mSize == Size::RIGHT ? style::DIVIDER_RIGHT : style::DIVIDER_LEFT, a);
            const F32 m = F32(style::DIVIDER_MARGIN);
            const F32 y = mDivider > 0 ? h - 1.f : 0.f;
            fillRect(LLRectf(m, y + 1.f, w - m, y), Fill::solid(line));
        }

        F32 x = pad;
        if (mIcon.notNull() && mSize != Size::SUB)
        {
            const S32 size = mSize == Size::RIGHT ? style::HEAD_ICON_RIGHT : style::HEAD_ICON;
            drawIcon(mIcon.get(), x + F32(size) * 0.5f, h * 0.5f, size, faded(style::HEAD_ICON_COLOR, a));
            x += F32(size + style::HEAD_ICON_GAP);
        }
        const F32 chevron_room = mCollapsible ? F32(style::HEAD_CHEVRON + 8) : 0.f;
        drawText(mFont, mLabelW, x, h * 0.5f, faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::VCENTER,
                 llmax(0, S32(w - x - pad - chevron_room)), true);
        if (mCollapsible)
        {
            const LLColor4 c = faded(mHover ? style::TEXT_LABEL : style::HEAD_CHEVRON_COLOR, a);
            drawIcon((mOpen ? mChevronUp : mChevronDown).get(), w - F32(style::HEAD_PAD) - F32(style::HEAD_CHEVRON) * 0.5f,
                     h * 0.5f, style::HEAD_CHEVRON, c);
            drawFocusRing(LLRectf(2.f, h - 2.f, w - 2.f, 2.f), 4.f);
        }
        drawSearchHighlight(localRectf());
    }

    // =========================================================================================
    // PsLabel

    PsLabel::Params::Params()
    :   kind("kind", "label"),
        align("align", "left")
    {
        mouse_opaque(false);
        tab_stop(false);
    }

    PsLabel::PsLabel(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_BODY)),
        mColor(style::LABEL),
        mAlign(parseAlign(p.align)),
        mAutoToolTip(!p.tool_tip.isProvided() || p.tool_tip().empty())
    {
        const std::string& k = p.kind;
        if (k == "white")       { mColor = style::TEXT_WHITE; }
        else if (k == "dim")    { mColor = style::TEXT_DIM; }
        else if (k == "sub")    { mColor = style::TEXT_SUB;  mFont = font(style::FONT_SMALL); }
        else if (k == "hint")   { mColor = style::TEXT_HINT; mFont = font(style::FONT_SMALL); }
        else if (k == "title")  { mColor = style::TEXT_WHITE; mFont = font(style::FONT_BUTTON); }
        updateToolTip();
    }

    void PsLabel::onLabelChanged()
    {
        updateToolTip();
    }

    void PsLabel::reshape(S32 width, S32 height, bool called_from_parent)
    {
        PsCtrl::reshape(width, height, called_from_parent);
        updateToolTip();
    }

    void PsLabel::updateToolTip()
    {
        // A label cut short shows its whole text as the tool tip (research/221 "文字").
        if (mAutoToolTip && mFont)
        {
            const bool cut = mFont->getWidth(mLabelW.c_str()) > getRect().getWidth();
            setToolTip(cut ? mLabel : std::string());
        }
    }

    void PsLabel::draw()
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        const F32 x = mAlign == LLFontGL::HCENTER ? w * 0.5f : mAlign == LLFontGL::RIGHT ? w : 0.f;
        drawText(mFont, mLabelW, x, h * 0.5f, faded(mColor, drawAlpha()), mAlign, LLFontGL::VCENTER, S32(w), true);
        drawSearchHighlight(localRectf());
    }

    // =========================================================================================
    // PsDivider

    PsDivider::Params::Params()
    :   kind("kind", "left"),
        margin("margin", style::DIVIDER_MARGIN)
    {
        mouse_opaque(false);
        tab_stop(false);
    }

    PsDivider::PsDivider(const Params& p)
    :   PsCtrl(p),
        mColor(p.kind() == "right" ? style::DIVIDER_RIGHT : style::DIVIDER_LEFT),
        mMargin(p.margin)
    {
    }

    void PsDivider::draw()
    {
        const F32 w = F32(getRect().getWidth());
        const F32 y = F32(getRect().getHeight() / 2);
        fillRect(LLRectf(F32(mMargin), y + 1.f, w - F32(mMargin), y), Fill::solid(faded(mColor, drawAlpha())));
    }

    // =========================================================================================
    // PsCheck

    PsCheck::Params::Params()
    {
        tab_stop(true);
    }

    PsCheck::PsCheck(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_BODY)),
        mMark(icon("check"))
    {
        if (p.initial_value.isProvided())
        {
            mOn = p.initial_value().asBoolean();
        }
    }

    void PsCheck::onClick(S32, S32)
    {
        mOn = !mOn;
        commitChange();
    }

    void PsCheck::draw()
    {
        look::check(localRectf(), mOn, mHover && getEnabled(), mPressed, drawAlpha(), mFont, mLabelW, mMark.get(), *this);
        drawSearchHighlight(localRectf());
    }

    // =========================================================================================
    // PsSwitch

    PsSwitch::Params::Params()
    {
        tab_stop(true);
    }

    PsSwitch::PsSwitch(const Params& p)
    :   PsCtrl(p)
    {
        if (p.initial_value.isProvided())
        {
            mOn = p.initial_value().asBoolean();
        }
    }

    void PsSwitch::setValue(const LLSD& value)
    {
        mOn = value.asBoolean();
        mMoving = false;        // set from outside: no movement
    }

    void PsSwitch::onClick(S32, S32)
    {
        mFrom = position();
        mOn = !mOn;
        mMoving = true;
        mTimer.reset();
        commitChange();
    }

    F32 PsSwitch::position() const
    {
        const F32 target = mOn ? 1.f : 0.f;
        if (!mMoving)
        {
            return target;
        }
        const F32 t = llclamp(mTimer.getElapsedTimeF32() / style::SWITCH_SECONDS, 0.f, 1.f);
        return mFrom + (target - mFrom) * t;
    }

    void PsSwitch::draw()
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        const F32 a = drawAlpha();
        const F32 pos = position();
        if (mMoving && pos == (mOn ? 1.f : 0.f))
        {
            mMoving = false;
        }
        const F32 sw = style::SWITCH_WIDTH;
        const F32 sh = style::SWITCH_HEIGHT;
        const F32 x0 = std::floor((w - sw) * 0.5f);
        const F32 y0 = std::floor((h - sh) * 0.5f);
        const LLRectf pill(x0, y0 + sh, x0 + sw, y0);
        LLColor4 fill = lerp(style::SWITCH_OFF, style::SWITCH_ON, pos);
        if (mHover && getEnabled())
        {
            fill = hovered(fill);
        }
        drawRound(pill, sh * 0.5f, Fill::solid(faded(fill, a)));
        const F32 kr = style::SWITCH_KNOB * 0.5f;
        const F32 left = x0 + style::SWITCH_KNOB_INSET + kr;
        const F32 right = x0 + sw - style::SWITCH_KNOB_INSET - kr;
        drawCircle(left + (right - left) * pos, y0 + sh * 0.5f, kr,
                   Fill::solid(faded(lerp(style::SWITCH_KNOB_OFF, style::SWITCH_KNOB_ON, pos), a)));
        drawSearchHighlight(localRectf());
        drawFocusRing(pill, sh * 0.5f);
    }

    // =========================================================================================
    // PsSlider

    PsSlider::Params::Params()
    :   min_value("min_value", 0.f),
        max_value("max_value", 1.f),
        increment("increment", 0.01f),
        decimal_digits("decimal_digits", 2),
        label_width("label_width", style::SLIDER_LABEL_WIDTH),
        value_width("value_width", style::VALUE_WIDTH),
        ticks("ticks", 0),
        unit("unit"),
        show_sign("show_sign", false),
        default_value("default_value")
    {
        tab_stop(true);
    }

    PsSlider::PsSlider(const Params& p)
    :   PsCtrl(p),
        mMin(p.min_value),
        mMax(llmax(p.min_value(), p.max_value())),
        mIncrement(llmax(0.f, p.increment())),
        mValue(p.min_value),
        mDecimals(llclamp(p.decimal_digits(), 0, 6)),
        mLabelWidth(llmax(0, p.label_width())),
        mValueWidth(llmax(0, p.value_width())),
        mTicks(llclamp(p.ticks(), 0, 64)),
        mUnit(p.unit),
        mShowSign(p.show_sign),
        mHasDefault(p.default_value.isProvided() && std::isfinite(p.default_value())),
        mDefaultValue(mHasDefault ? llclamp(p.default_value(), mMin, mMax) : 0.f),
        mDefaultEps(llmax(1e-5f * (mMax - mMin), 1e-6f)),
        mFont(font(style::FONT_BODY))
    {
        if (mIncrement > 0.f)
        {
            // A step away from the default is never "at the default" (the "D" button stays live for it).
            mDefaultEps = llmin(mDefaultEps, 0.25f * mIncrement);
        }
        if (p.initial_value.isProvided())
        {
            mValue = snap(F32(p.initial_value().asReal()));
        }
        updateText();
    }

    PsSlider::~PsSlider()
    {
        // A value being typed is dropped with the slider. The editor, our child, is to lose the focus here, while
        // this slider is whole: later (~LLUICtrl) its focus-lost callback would run endEdit() on what is left of us.
        mEditing = false;       // endEdit() returns at once
        if (mEditor)
        {
            gFocusMgr.releaseFocusIfNeeded(mEditor);
        }
    }

    bool PsSlider::isDefault(F32 v) const
    {
        return mHasDefault && std::fabs(v - mDefaultValue) <= mDefaultEps;
    }

    F32 PsSlider::snap(F32 v) const
    {
        v = llclamp(v, mMin, mMax);
        if (isDefault(v))
        {
            // A shipped default off the increment (RenderGlowStrength 0.325 on a 0.05 step, CameraAngle's 60
            // degrees on a field of view from 5.7 by 0.5) is shown as it is: read back after the "D" button,
            // it would otherwise be moved to the next step and look changed.
            return mDefaultValue;
        }
        if (mIncrement > 0.f)
        {
            v = mMin + std::round((v - mMin) / mIncrement) * mIncrement;
            v = llclamp(v, mMin, mMax);
        }
        return v;
    }

    bool PsSlider::setValueInternal(F32 v)
    {
        v = snap(v);
        if (v == mValue)
        {
            return false;
        }
        mValue = v;
        updateText();
        return true;
    }

    void PsSlider::setValue(const LLSD& value)
    {
        setValueInternal(F32(value.asReal()));
    }

    void PsSlider::setDisplayText(const std::string& text)
    {
        if (text != mDisplay)
        {
            mDisplay = text;
            updateText();
        }
    }

    void PsSlider::updateText()
    {
        // Only when the value (or the shown text) changes.
        if (!mDisplay.empty())
        {
            mValueText = utf8str_to_wstring(mDisplay);
            return;
        }
        F32 v = mValue;
        if (std::fabs(v) < 0.5f * std::pow(10.f, -F32(mDecimals)))
        {
            v = 0.f;    // no "-0.0"
        }
        std::string text = llformat("%.*f", mDecimals, v);
        if (mShowSign && v > 0.f)
        {
            text = "+" + text;
        }
        mValueText = utf8str_to_wstring(text + mUnit);
    }

    F32 PsSlider::trackLeft() const
    {
        return F32(mLabelWidth) + style::SLIDER_KNOB * 0.5f;
    }

    F32 PsSlider::trackRight() const
    {
        F32 end = F32(getRect().getWidth());
        if (mHasDefault)
        {
            // the value box's own gap comes next; with no value box, the track's
            end -= F32(style::DEFAULT_BTN_WIDTH + (mValueWidth > 0 ? style::DEFAULT_BTN_GAP : style::SLIDER_GAP));
        }
        if (mValueWidth > 0)
        {
            end -= F32(mValueWidth) + style::SLIDER_GAP;
        }
        return llmax(trackLeft(), end - style::SLIDER_KNOB * 0.5f);
    }

    F32 PsSlider::knobX() const
    {
        const F32 t = mMax > mMin ? (mValue - mMin) / (mMax - mMin) : 0.f;
        return trackLeft() + (trackRight() - trackLeft()) * t;
    }

    LLRectf PsSlider::valueRect() const
    {
        F32 w = F32(getRect().getWidth());
        if (mHasDefault)
        {
            w -= F32(style::DEFAULT_BTN_WIDTH + style::DEFAULT_BTN_GAP);
        }
        const F32 mid = std::floor(F32(getRect().getHeight()) * 0.5f);
        const F32 vh = F32(style::VALUE_HEIGHT) * 0.5f;
        return LLRectf(w - F32(mValueWidth), mid + vh, w, mid - vh);
    }

    LLRectf PsSlider::defaultRect() const
    {
        if (!mHasDefault)
        {
            return LLRectf(0.f, 0.f, 0.f, 0.f);
        }
        const F32 w = F32(getRect().getWidth());
        const F32 mid = std::floor(F32(getRect().getHeight()) * 0.5f);
        const F32 bw = F32(style::DEFAULT_BTN_WIDTH);
        const F32 bh = F32(style::VALUE_HEIGHT);
        return LLRectf(w - bw, mid + bh * 0.5f, w, mid - bh * 0.5f);
    }

    void PsSlider::drawDefaultButton(F32 alpha) const
    {
        static const LLWString D_LABEL = utf8str_to_wstring("D");
        const LLRectf d = defaultRect();
        const bool atDefault = isDefault(mValue);
        const bool hot = mDefaultHover && getEnabled() && !atDefault;
        if (hot)
        {
            drawRound(d, style::VALUE_RADIUS, Fill::solid(faded(style::DEFAULT_BTN_FILL_HOVER, alpha)));
        }
        const LLColor4& color = atDefault ? style::SLIDER_EMPTY
                               : hot       ? style::DEFAULT_BTN_COLOR_HOVER
                                           : style::DEFAULT_BTN_COLOR;
        drawText(mFont, D_LABEL, d.getCenterX(), d.getCenterY(), faded(color, alpha), LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    void PsSlider::reshape(S32 width, S32 height, bool called_from_parent)
    {
        PsCtrl::reshape(width, height, called_from_parent);
        if (mEditor)
        {
            const LLRectf v = valueRect();
            mEditor->setRect(LLRect(S32(v.mLeft), S32(v.mTop), S32(v.mRight), S32(v.mBottom)));
        }
    }

    void PsSlider::draw()
    {
        const F32 h = F32(getRect().getHeight());
        const F32 a = drawAlpha();
        const F32 mid = std::floor(h * 0.5f);

        if (mLabelWidth > 0)
        {
            drawText(mFont, mLabelW, 0.f, mid, faded(style::LABEL, a), LLFontGL::LEFT, LLFontGL::VCENTER,
                     mLabelWidth - 6, true);
        }

        look::sliderTrack(trackLeft(), trackRight(), knobX(), mid, mTicks, mDragging, mHover && getEnabled(), a, *this);
        if (mValueWidth > 0)
        {
            const LLRectf v = valueRect();
            look::valueBox(v, a);
            if (!mEditing)
            {
                look::valueText(v, mFont, mValueText, a);
            }
        }
        if (mHasDefault)
        {
            drawDefaultButton(a);
        }
        drawSearchHighlight(localRectf());
        LLView::draw();     // the value editor while typing
    }

    bool PsSlider::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        if (mEditing && childrenHandleMouseDown(x, y, mask))
        {
            return true;
        }
        if (!getEnabled())
        {
            return true;
        }
        if (mValueWidth > 0 && valueRect().pointInRect(F32(x), F32(y)))
        {
            beginEdit();
            return true;
        }
        if (mHasDefault && defaultRect().pointInRect(F32(x), F32(y)))
        {
            setFocus(true);
            if (mValue != mDefaultValue)
            {
                mValue = mDefaultValue;     // as it is, not snapped to the increment (snap() keeps it too)
                updateText();
                commitChange();
            }
            return true;
        }
        setFocus(true);
        mDragging = true;
        gFocusMgr.setMouseCapture(this);
        const F32 span = trackRight() - trackLeft();
        if (span > 0.f && setValueInternal(mMin + (mMax - mMin) * (F32(x) - trackLeft()) / span))
        {
            commitChange();
        }
        return true;
    }

    bool PsSlider::handleHover(S32 x, S32 y, MASK mask)
    {
        if (mDragging && hasMouseCapture())
        {
            const F32 span = trackRight() - trackLeft();
            if (span > 0.f && setValueInternal(mMin + (mMax - mMin) * (F32(x) - trackLeft()) / span))
            {
                commitChange();
            }
            return true;
        }
        if (mHasDefault)
        {
            mDefaultHover = defaultRect().pointInRect(F32(x), F32(y));
        }
        return PsCtrl::handleHover(x, y, mask);
    }

    bool PsSlider::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        mDragging = false;
        return true;
    }

    void PsSlider::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mDefaultHover = false;
        PsCtrl::onMouseLeave(x, y, mask);
    }

    bool PsSlider::handleToolTip(S32 x, S32 y, MASK mask)
    {
        if (!mHasDefault || !defaultRect().pointInRect(F32(x), F32(y)))
        {
            return PsCtrl::handleToolTip(x, y, mask);     // the row's own tip
        }
        const LLRectf d = defaultRect();
        LLRect screen;
        localRectToScreen(LLRect(S32(d.mLeft), S32(d.mTop), S32(d.mRight), S32(d.mBottom)), &screen);
        LLToolTipMgr::instance().show(LLToolTip::Params().message(LLTrans::getString("PlusPsResetDefault")).sticky_rect(screen));
        return true;
    }

    F32 PsSlider::stepFrom(F32 steps, F32 step) const
    {
        if (mIncrement > 0.f && isDefault(mValue))
        {
            // A default off the increment (0.325 on a 0.05 step): the first step lands on the nearest step in
            // the direction pressed (0.35 or 0.3), not past it after snap()'s rounding (0.4).
            const F32 q = (mValue - mMin) / mIncrement;
            if (std::fabs(q - std::round(q)) > 1e-3f)
            {
                const F32 n = steps > 0.f ? std::ceil(q) + (steps - 1.f) : std::floor(q) + (steps + 1.f);
                return mMin + n * mIncrement;
            }
        }
        return mValue + steps * step;
    }

    bool PsSlider::handleKeyHere(KEY key, MASK mask)
    {
        if (!getEnabled())
        {
            return false;
        }
        const F32 step = mIncrement > 0.f ? mIncrement : (mMax - mMin) / 100.f;
        const F32 steps = (mask & MASK_SHIFT) ? 10.f : 1.f;
        F32 v = mValue;
        switch (key)
        {
        case KEY_LEFT:  case KEY_DOWN: v = stepFrom(-steps, step); break;
        case KEY_RIGHT: case KEY_UP:   v = stepFrom(steps, step); break;
        case KEY_HOME:  v = mMin; break;
        case KEY_END:   v = mMax; break;
        default:        return LLUICtrl::handleKeyHere(key, mask);
        }
        if (setValueInternal(v))
        {
            commitChange();
        }
        return true;
    }

    void PsSlider::beginEdit()
    {
        if (!mEditor)
        {
            // Made on the first click into the value box (not while drawing).
            LLLineEditor::Params lp(LLUICtrlFactory::getDefaultParams<LLLineEditor>());
            const LLRectf v = valueRect();
            lp.name("value_editor");
            lp.rect(LLRect(S32(v.mLeft), S32(v.mTop), S32(v.mRight), S32(v.mBottom)));
            lp.font(mFont);
            lp.max_length.bytes(24);
            lp.commit_on_focus_lost(true);
            lp.select_on_focus(true);
            lp.revert_on_esc(true);
            lp.use_bg_color(true);
            lp.bg_color(LLUIColor(LLColor4::transparent));
            lp.text_color(LLUIColor(style::TEXT_WHITE));
            lp.cursor_color(LLUIColor(style::TEXT_WHITE));
            lp.draw_focus_border(false);
            lp.border.visible(false);
            lp.text_pad_left(style::VALUE_PAD);
            lp.text_pad_right(style::VALUE_PAD);
            lp.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP);
            mEditor = LLUICtrlFactory::create<LLLineEditor>(lp);
            mEditor->setVisible(false);
            addChild(mEditor);
            mEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { endEdit(); });
            // The editor commits on losing the focus only when the text changed: close it then too.
            mEditor->setFocusLostCallback([this](LLFocusableElement*) { endEdit(); });
        }
        mEditing = true;
        mEditor->setText(llformat("%.*f", mDecimals, mValue));
        mEditor->setVisible(true);
        mEditor->setFocus(true);
        mEditor->selectAll();
    }

    void PsSlider::endEdit()
    {
        if (!mEditing || !mEditor)
        {
            return;
        }
        mEditing = false;       // first: hiding the editor loses its focus and commits once more
        const std::string text = mEditor->getText();
        char* end = nullptr;
        const F32 v = std::strtof(text.c_str(), &end);
        if (end != text.c_str() && std::isfinite(v) && setValueInternal(v))
        {
            commitChange();
        }
        mEditor->setVisible(false);
    }

    // =========================================================================================
    // PsCombo

    PsCombo::Params::Params()
    {
        combo_list.bg_writeable_color(LLUIColor(style::LIST_BG));
        combo_list.bg_readonly_color(LLUIColor(style::LIST_BG));
        combo_list.bg_selected_color(LLUIColor(style::LIST_SELECTED));
        combo_list.hovered_color(LLUIColor(hovered(style::LIST_BG)));
        combo_list.highlighted_color(LLUIColor(style::LIST_SELECTED));
        combo_list.fg_unselected_color(LLUIColor(style::TEXT_WHITE));
        combo_list.fg_selected_color(LLUIColor(style::TEXT_WHITE));
    }

    PsCombo::PsCombo(const Params& p)
    :   LLComboBox(p),
        mFont(font(style::FONT_BODY)),
        mChevron(icon("chevron_down"))
    {
    }

    void PsCombo::onMouseEnter(S32 x, S32 y, MASK mask)
    {
        mHover = true;
        LLComboBox::onMouseEnter(x, y, mask);
    }

    void PsCombo::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHover = false;
        LLComboBox::onMouseLeave(x, y, mask);
    }

    void PsCombo::refreshShown()
    {
        mShownIndex = -2;
    }

    void PsCombo::draw()
    {
        // Two integers a frame; the text is read again only when they change.
        const S32 index = getCurrentIndex();
        const S32 count = getItemCount();
        if (index != mShownIndex || count != mShownCount)
        {
            mShownIndex = index;
            mShownCount = count;
            mShown = utf8str_to_wstring(getSelectedItemLabel());
        }

        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        const F32 a = getDrawContext().mAlpha * (getEnabled() ? 1.f : style::STATE_DISABLED);
        const LLRectf r(0.f, h, w, 0.f);
        const LLColor4 fill = mHover && getEnabled() ? hovered(style::COMBO_FILL) : style::COMBO_FILL;
        drawRoundBordered(r, style::COMBO_RADIUS, Fill::solid(faded(fill, a)), faded(style::COMBO_BORDER, a));
        const F32 chevron_room = F32(style::COMBO_CHEVRON + style::COMBO_PAD + 4);
        drawText(mFont, mShown, F32(style::COMBO_PAD), h * 0.5f, faded(style::TEXT_WHITE, a), LLFontGL::LEFT,
                 LLFontGL::VCENTER, llmax(0, S32(w - style::COMBO_PAD - chevron_room)), true);
        drawIcon(mChevron.get(), w - F32(style::COMBO_PAD) - F32(style::COMBO_CHEVRON) * 0.5f, h * 0.5f,
                 style::COMBO_CHEVRON, faded(style::ICON_LIGHT, a));
        if (gFocusMgr.childHasKeyboardFocus(this) && !(mList && mList->getVisible()))
        {
            const LLRectf ring(-2.f, h + 2.f, w + 2.f, -2.f);
            drawRing(ring, style::COMBO_RADIUS + 2.f, 1.f, faded(style::FOCUS_RING, getDrawContext().mAlpha));
        }
        if (getHighlighted())   // the search (LLComboBox is a SearchableControl; its button, which would show it, is not drawn)
        {
            look::searchHit(r, getDrawContext().mAlpha);
        }
        // The official button keeps the clicks (it is not drawn); the open list is drawn as it is.
        if (mList && mList->getVisible())
        {
            drawChild(mList);
        }
    }
}
