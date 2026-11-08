/**
 * @file pscards.cpp
 * @brief The photo screen's widgets: the world's overlay, cards, pictures and the shutter strip (research/221).
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

#include "pswidgets.h"
#include "pscompat.h"
#include "psstyle.h"

#include "llkeyboard.h"
#include "llrender.h"
#include "lltexture.h"
#include "lluictrlfactory.h"

#include <cmath>

namespace plus::psui
{
    namespace
    {
        LLDefaultChildRegistry::Register<PsChip>       sChipRegistration("plus_ps_chip");
        LLDefaultChildRegistry::Register<PsIconButton> sIconButtonRegistration("plus_ps_icon_button");
        LLDefaultChildRegistry::Register<PsHistogram>  sHistogramRegistration("plus_ps_histogram");
        LLDefaultChildRegistry::Register<PsReadout>    sReadoutRegistration("plus_ps_readout");
        LLDefaultChildRegistry::Register<PsCard>       sCardRegistration("plus_ps_card");
        LLDefaultChildRegistry::Register<PsPicture>    sPictureRegistration("plus_ps_picture");
        LLDefaultChildRegistry::Register<PsPresetCard> sPresetCardRegistration("plus_ps_preset_card");
        LLDefaultChildRegistry::Register<PsAddCard>    sAddCardRegistration("plus_ps_add_card");
        LLDefaultChildRegistry::Register<PsSegment>    sSegmentRegistration("plus_ps_segment");
        LLDefaultChildRegistry::Register<PsShutter>    sShutterRegistration("plus_ps_shutter");
        LLDefaultChildRegistry::Register<PsButton>     sButtonRegistration("plus_ps_button");
        LLDefaultChildRegistry::Register<PsTab>        sTabRegistration("plus_ps_tab");
        LLDefaultChildRegistry::Register<PsListRow>    sListRowRegistration("plus_ps_list_row");

        // A width that changed inside a row moves the row's other children.
        void rearrangeParent(LLView* view)
        {
            if (PsRow* row = dynamic_cast<PsRow*>(view->getParent()))
            {
                row->arrange();
            }
        }

        // A filled curve from the base line up to heights[i] * height (untextured).
        void drawArea(const std::array<F32, PsHistogram::BINS>& heights, const LLRectf& r, const LLColor4& color)
        {
            const F32 step = r.getWidth() / F32(PsHistogram::BINS - 1);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(color.mV);
            for (int i = 0; i + 1 < PsHistogram::BINS; ++i)
            {
                const F32 xa = r.mLeft + step * F32(i);
                const F32 xb = xa + step;
                const F32 ya = r.mBottom + heights[i] * r.getHeight();
                const F32 yb = r.mBottom + heights[i + 1] * r.getHeight();
                gGL.vertex2f(xa, r.mBottom);
                gGL.vertex2f(xb, r.mBottom);
                gGL.vertex2f(xb, yb);
                gGL.vertex2f(xa, r.mBottom);
                gGL.vertex2f(xb, yb);
                gGL.vertex2f(xa, ya);
            }
            gGL.end();
        }

        // The picture, or a dark ground with the placeholder icon.
        void drawPictureOrPlaceholder(LLTexture* texture, const LLUIImage* placeholder, const LLRectf& r, F32 radius,
                                      U8 corners, F32 alpha, PictureFit fit = PictureFit::Crop)
        {
            const bool ready = texture && texture->getWidth(0) > 0 && texture->getHeight(0) > 0;
            if (ready)
            {
                drawPicture(texture, r, radius, faded(LLColor4::white, alpha), corners, fit);
                return;
            }
            drawRound(r, radius, Fill::solid(faded(style::PLACEHOLDER, alpha)), corners);
            const S32 size = S32(llmin(r.getWidth(), r.getHeight()) * 0.42f);
            drawIcon(placeholder, r.getCenterX(), r.getCenterY(), size, faded(style::PLACEHOLDER_ICON, alpha));
        }
    }

    // =========================================================================================
    // PsChip

    PsChip::Params::Params()
    :   dropdown("dropdown", false),
        auto_width("auto_width", true)
    {
        tab_stop(true);
    }

    PsChip::PsChip(const Params& p)
    :   PsCtrl(p),
        mDropdown(p.dropdown),
        mAutoWidth(p.auto_width),
        mFont(font(style::FONT_BUTTON)),
        mChevron(icon("chevron_down"))
    {
        fitWidth();
    }

    void PsChip::onLabelChanged()
    {
        fitWidth();
    }

    void PsChip::fitWidth()
    {
        if (!mAutoWidth || !mFont)
        {
            return;
        }
        S32 w = style::CHIP_PAD * 2 + mFont->getWidth(mLabelW.c_str());
        if (mIcon.notNull())
        {
            w += style::CHIP_ICON + style::CHIP_GAP;
        }
        if (mDropdown)
        {
            w += style::CHIP_GAP + style::COMBO_CHEVRON;
        }
        if (w != getRect().getWidth())
        {
            reshape(w, getRect().getHeight());
            rearrangeParent(this);
        }
    }

    void PsChip::draw()
    {
        const LLRectf r = localRectf();
        const F32 h = r.getHeight();
        const F32 a = drawAlpha();
        LLColor4 fill = style::CHIP_FILL;
        if (mPressed)                    fill = pressed(fill);
        else if (mHover && getEnabled()) fill = lerp(fill, LLColor4(1.f, 1.f, 1.f, fill.mV[VALPHA]), 0.08f);
        drawRound(r, style::CHIP_RADIUS, Fill::solid(faded(fill, a)));
        F32 x = F32(style::CHIP_PAD);
        if (mIcon.notNull())
        {
            drawIcon(mIcon.get(), x + F32(style::CHIP_ICON) * 0.5f, h * 0.5f, style::CHIP_ICON, faded(style::CHIP_ICON_COLOR, a));
            x += F32(style::CHIP_ICON + style::CHIP_GAP);
        }
        const F32 right_room = F32(style::CHIP_PAD) + (mDropdown ? F32(style::CHIP_GAP + style::COMBO_CHEVRON) : 0.f);
        drawText(mFont, mLabelW, x, h * 0.5f, faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::VCENTER,
                 llmax(0, S32(r.getWidth() - x - right_room)), true);
        if (mDropdown)
        {
            drawIcon(mChevron.get(), r.getWidth() - F32(style::CHIP_PAD) - F32(style::COMBO_CHEVRON) * 0.5f, h * 0.5f,
                     style::COMBO_CHEVRON, faded(style::CHIP_ICON_COLOR, a));
        }
        drawSearchHighlight(r);
        drawFocusRing(r, style::CHIP_RADIUS);
    }

    // =========================================================================================
    // PsIconButton

    PsIconButton::Params::Params()
    :   icon_size("icon_size", style::ICONBTN_ICON),
        toggle("toggle", false),
        star("star", false)
    {
        tab_stop(true);
    }

    PsIconButton::PsIconButton(const Params& p)
    :   PsCtrl(p),
        mIconSize(p.star && !p.icon_size.isProvided() ? style::LIST_STAR : p.icon_size()),
        mToggle(p.toggle),
        mStar(p.star)
    {
        if (p.initial_value.isProvided())
        {
            mOn = p.initial_value().asBoolean();
        }
        if (mStar)
        {
            mIcon       = icon("star");
            mStarFilled = icon("star_filled");
        }
    }

    void PsIconButton::onClick(S32, S32)
    {
        if (mToggle)
        {
            mOn = !mOn;
        }
        commitChange();
    }

    void PsIconButton::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        const F32 cx = r.getCenterX();
        const F32 cy = r.getCenterY();
        if (mStar)
        {
            // as the list rows' star (19.): filled and yellow while on, grey and lighter under the mouse while off;
            // no hover circle (the star sits in a row's narrow strip)
            const LLColor4& color = mOn ? style::LIST_STAR_ON : (mHover && getEnabled() ? style::ICON_LIGHT : style::LIST_STAR_OFF);
            drawIcon((mOn ? mStarFilled : mIcon).get(), cx, cy, mIconSize, faded(color, a));
            const F32 half = std::floor(llmin(r.getWidth(), r.getHeight()) * 0.5f);
            drawSearchHighlight(r);
            drawFocusRing(LLRectf(cx - half, cy + half, cx + half, cy - half), half);
            return;
        }
        if ((mHover || mPressed) && getEnabled())
        {
            const LLColor4 circle = mPressed ? faded(style::ICONBTN_HOVER_COLOR, 1.6f) : style::ICONBTN_HOVER_COLOR;
            drawCircle(cx, cy, style::ICONBTN_HOVER * 0.5f, Fill::solid(faded(circle, a)));
        }
        drawIcon(mIcon.get(), cx, cy, mIconSize, faded(mOn ? style::ACCENT_LINE : style::ICONBTN_COLOR, a));
        drawSearchHighlight(r);
        drawFocusRing(LLRectf(cx - style::ICONBTN_HOVER * 0.5f, cy + style::ICONBTN_HOVER * 0.5f,
                              cx + style::ICONBTN_HOVER * 0.5f, cy - style::ICONBTN_HOVER * 0.5f), style::ICONBTN_HOVER * 0.5f);
    }

    // =========================================================================================
    // PsHistogram

    PsHistogram::Params::Params()
    {
        mouse_opaque(false);
        tab_stop(false);
    }

    PsHistogram::PsHistogram(const Params& p)
    :   PsCtrl(p)
    {
    }

    void PsHistogram::setData(const F32* red, const F32* green, const F32* blue, S32 count)
    {
        if (!red || !green || !blue || count < 2)
        {
            mHasData = false;
            return;
        }
        // Resample to BINS (linear between the given levels), then scale so the highest is 1.
        F32 peak = 0.f;
        for (int i = 0; i < BINS; ++i)
        {
            const F32 pos = F32(i) * F32(count - 1) / F32(BINS - 1);
            const S32 i0 = llclamp(S32(pos), 0, count - 1);
            const S32 i1 = llmin(i0 + 1, count - 1);
            const F32 t = pos - F32(i0);
            mRed[i]   = llmax(0.f, red[i0]   + (red[i1]   - red[i0])   * t);
            mGreen[i] = llmax(0.f, green[i0] + (green[i1] - green[i0]) * t);
            mBlue[i]  = llmax(0.f, blue[i0]  + (blue[i1]  - blue[i0])  * t);
            mLuma[i]  = 0.2126f * mRed[i] + 0.7152f * mGreen[i] + 0.0722f * mBlue[i];
            peak = llmax(peak, llmax(mRed[i], llmax(mGreen[i], mBlue[i])));
        }
        const F32 scale = peak > 0.f ? 1.f / peak : 0.f;
        for (int i = 0; i < BINS; ++i)
        {
            mRed[i] *= scale;
            mGreen[i] *= scale;
            mBlue[i] *= scale;
            mLuma[i] *= scale;
        }
        mHasData = true;
    }

    void PsHistogram::draw()
    {
        if (!mHasData)
        {
            return;     // no frame without data (research/221 9.)
        }
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        drawRoundBordered(r, style::HIST_RADIUS, Fill::solid(faded(style::HIST_FILL, a)), faded(style::HIST_BORDER, a));
        const F32 pad = F32(style::HIST_PAD);
        const LLRectf inner(r.mLeft + pad, r.mTop - pad, r.mRight - pad, r.mBottom + pad);
        drawArea(mLuma, inner, faded(style::HIST_LUMA, a));
        drawArea(mRed, inner, faded(style::HIST_R, a));
        drawArea(mGreen, inner, faded(style::HIST_G, a));
        drawArea(mBlue, inner, faded(style::HIST_B, a));
        fillRect(LLRectf(inner.mLeft, inner.mBottom + 1.f, inner.mRight, inner.mBottom), Fill::solid(faded(style::HIST_BASE, a)));
    }

    // =========================================================================================
    // PsReadout

    PsReadout::Params::Params()
    {
        mouse_opaque(false);
        tab_stop(false);
    }

    PsReadout::PsReadout(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_READOUT))
    {
        // The label holds the first items, joined by '|'.
        size_t start = 0;
        for (int i = 0; i < ITEMS && start <= mLabel.size(); ++i)
        {
            const size_t bar = mLabel.find('|', start);
            setItem(i, mLabel.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
            if (bar == std::string::npos)
            {
                break;
            }
            start = bar + 1;
        }
    }

    void PsReadout::setItem(int index, const std::string& text)
    {
        if (index < 0 || index >= ITEMS || mItems[index] == text)
        {
            return;
        }
        mItems[index] = text;
        mItemsW[index] = utf8str_to_wstring(text);
        mWidths[index] = mFont ? mFont->getWidth(mItemsW[index].c_str()) : 0;
    }

    void PsReadout::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        fillRect(r, Fill::vertical(LLColor4(0.f, 0.f, 0.f, 0.f), faded(style::READOUT_SHADE, a)));
        F32 x = F32(style::READOUT_LEFT);
        for (int i = 0; i < ITEMS; ++i)
        {
            if (mItemsW[i].empty())
            {
                continue;
            }
            drawText(mFont, mItemsW[i], x, F32(style::READOUT_BOTTOM), faded(style::READOUT_TEXT, a),
                     LLFontGL::LEFT, LLFontGL::BOTTOM);
            x += F32(mWidths[i] + style::READOUT_GAP);
        }
    }

    // =========================================================================================
    // PsCard

    PsCard::Params::Params()
    :   kind("kind", "thumb"),
        subtitle("subtitle"),
        more("more", true),
        image("image")
    {
        tab_stop(true);
    }

    PsCard::PsCard(const Params& p)
    :   PsCtrl(p),
        mAvatar(p.kind() == "avatar"),
        mMore(p.more.isProvided() ? p.more() : p.kind() != "avatar"),
        mTitleFont(font(style::FONT_BUTTON)),
        mSubFont(font(style::FONT_BODY)),
        mMoreIcon(icon("more"))
    {
        setSubtitle(p.subtitle);
        if (p.image.isProvided() && p.image())
        {
            mTexture = p.image()->getImage();
        }
    }

    void PsCard::setSubtitle(const std::string& text)
    {
        if (text != mSubtitle)
        {
            mSubtitle = text;
            mSubtitleW = utf8str_to_wstring(text);
        }
    }

    void PsCard::draw()
    {
        const LLRectf r = localRectf();
        const F32 w = r.getWidth();
        const F32 h = r.getHeight();
        const F32 a = drawAlpha();
        LLRectf pic;
        if (mAvatar)
        {
            pic = LLRectf(0.f, h, h, 0.f);
            if (mHover && getEnabled())
            {
                drawRound(r, style::CARD_RADIUS, Fill::solid(faded(style::RAIL_HOVER, a)));
            }
            drawPictureOrPlaceholder(mTexture.get(), mIcon.get(), pic, style::CARD_RADIUS, CORNERS_ALL, a);
        }
        else
        {
            LLColor4 fill = style::CARD_FILL;
            if (mPressed)                    fill = pressed(fill);
            else if (mHover && getEnabled()) fill = hovered(fill);
            drawRoundBordered(r, style::CARD_RADIUS, Fill::solid(faded(fill, a)), faded(style::CARD_BORDER, a));
            pic = LLRectf(1.f, h - 1.f, 1.f + F32(style::CARD_PICTURE_WIDTH), 1.f);
            drawPictureOrPlaceholder(mTexture.get(), mIcon.get(), pic, style::CARD_RADIUS - 1.f, CORNERS_LEFT, a);
            if (mChosen)
            {
                drawRing(r, style::CARD_RADIUS, 2.f, faded(style::PRESET_SELECTED_BORDER, a));
            }
        }

        const F32 x = pic.mRight + F32(style::CARD_TEXT_GAP);
        const F32 more_room = mMore ? F32(style::CARD_MORE_ICON + 2 * style::HEAD_PAD) : F32(style::HEAD_PAD);
        const S32 room = llmax(0, S32(w - x - more_room));
        if (mSubtitleW.empty())
        {
            drawText(mTitleFont, mLabelW, x, h * 0.5f, faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::VCENTER, room, true);
        }
        else
        {
            drawText(mTitleFont, mLabelW, x, h * 0.5f + 1.f, faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::BOTTOM, room, true);
            drawText(mSubFont, mSubtitleW, x, h * 0.5f - 2.f, faded(style::TEXT_SUB, a), LLFontGL::LEFT, LLFontGL::TOP, room, true);
        }
        if (mMore)
        {
            drawIcon(mMoreIcon.get(), w - F32(style::HEAD_PAD) - F32(style::CARD_MORE_ICON) * 0.5f, h * 0.5f,
                     style::CARD_MORE_ICON, faded(style::ICON_LIGHT, a));
        }
        drawSearchHighlight(r);
        drawFocusRing(r, style::CARD_RADIUS);
    }

    // =========================================================================================
    // PsPicture

    PsPicture::Params::Params()
    :   image("image"),
        radius("radius", style::CARD_RADIUS),
        placeholder("placeholder", "icon")
    {
        mouse_opaque(false);
        tab_stop(false);
    }

    PsPicture::PsPicture(const Params& p)
    :   PsCtrl(p),
        mRadius(p.radius),
        mDuskNight(p.placeholder() == "dusk_night")
    {
        if (p.image.isProvided() && p.image())
        {
            mTexture = p.image()->getImage();
        }
    }

    void PsPicture::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        const bool ready = mTexture.notNull() && mTexture->getWidth(0) > 0 && mTexture->getHeight(0) > 0;
        if (!ready && mDuskNight)
        {
            // Until there are pictures: an evening sky on the left, the night on the right.
            const F32 mid = r.getCenterX();
            drawRound(LLRectf(r.mLeft, r.mTop, mid, r.mBottom), mRadius,
                      Fill::vertical(faded(rgb(0x3d4f7c), a), faded(rgb(0xd88a5a), a)), CORNERS_LEFT);
            drawRound(LLRectf(mid, r.mTop, r.mRight, r.mBottom), mRadius,
                      Fill::vertical(faded(rgb(0x121a33), a), faded(rgb(0x2a3150), a)), CORNERS_RIGHT);
            return;
        }
        drawPictureOrPlaceholder(mTexture.get(), mIcon.get(), r, mRadius, CORNERS_ALL, a, mFit);
    }

    // =========================================================================================
    // PsPresetCard

    PsPresetCard::Params::Params()
    :   subtitle("subtitle"),
        image("image"),
        deletable("deletable", false)
    {
        tab_stop(true);
    }

    PsPresetCard::PsPresetCard(const Params& p)
    :   PsCtrl(p),
        mDeletable(p.deletable),
        mTitleFont(font(style::FONT_BUTTON)),
        mSubFont(font(style::FONT_SMALL)),
        mClose(icon("close"))
    {
        setSubtitle(p.subtitle);
        if (p.image.isProvided() && p.image())
        {
            mTexture = p.image()->getImage();
        }
        if (p.initial_value.isProvided())
        {
            mChosen = p.initial_value().asBoolean();
        }
    }

    void PsPresetCard::setSubtitle(const std::string& text)
    {
        if (text != mSubtitle)
        {
            mSubtitle = text;
            mSubtitleW = utf8str_to_wstring(text);
        }
    }

    LLRectf PsPresetCard::pictureRect() const
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        return LLRectf(0.f, h, w, h - F32(style::PRESET_PICTURE_HEIGHT));
    }

    LLRectf PsPresetCard::closeRect() const
    {
        const LLRectf pic = pictureRect();
        const F32 s = F32(style::PRESET_CLOSE);
        return LLRectf(pic.mRight - 5.f - s, pic.mTop - 5.f, pic.mRight - 5.f, pic.mTop - 5.f - s);
    }

    void PsPresetCard::onClick(S32 x, S32 y)
    {
        if (mDeletable && mOnDelete && x >= 0 && closeRect().pointInRect(F32(x), F32(y)))
        {
            mOnDelete(this);
            return;
        }
        commitChange();
    }

    void PsPresetCard::draw()
    {
        const F32 a = drawAlpha();
        const LLRectf pic = pictureRect();
        const bool hover = mHover && getEnabled();
        drawPictureOrPlaceholder(mTexture.get(), mIcon.get(), pic, style::CARD_RADIUS, CORNERS_ALL, a);
        if (hover)
        {
            drawRound(pic, style::CARD_RADIUS, Fill::solid(faded(LLColor4(1.f, 1.f, 1.f, 0.03f), a)));
        }
        if (mChosen)
        {
            drawRing(pic, style::CARD_RADIUS, 2.f, faded(style::PRESET_SELECTED_BORDER, a));
        }
        else if (hover)
        {
            drawRing(pic, style::CARD_RADIUS, 1.f, faded(style::PRESET_HOVER_BORDER, a));
        }
        if (hover && mDeletable)
        {
            const LLRectf c = closeRect();
            drawCircle(c.getCenterX(), c.getCenterY(), c.getWidth() * 0.5f, Fill::solid(faded(style::PRESET_CLOSE_FILL, a)));
            drawIcon(mClose.get(), c.getCenterX(), c.getCenterY(), style::PRESET_CLOSE - 6, faded(LLColor4::white, a));
        }

        const S32 room = getRect().getWidth();
        const F32 ty = pic.mBottom - F32(style::PRESET_TEXT_GAP);
        drawText(mTitleFont, mLabelW, 0.f, ty, faded(style::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::TOP, room, true);
        const F32 sy = ty - F32(mTitleFont ? mTitleFont->getLineHeight() : 17) - 1.f;
        drawText(mSubFont, mSubtitleW, 0.f, sy, faded(style::TEXT_DIM, a), LLFontGL::LEFT, LLFontGL::TOP, room, true);
        drawSearchHighlight(pic);
        drawFocusRing(pic, style::CARD_RADIUS);
    }

    // =========================================================================================
    // PsAddCard

    PsAddCard::Params::Params()
    {
        tab_stop(true);
    }

    PsAddCard::PsAddCard(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_SMALL))
    {
        if (mIcon.isNull())
        {
            mIcon = icon("plus");
        }
    }

    void PsAddCard::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        LLColor4 fill = style::ADD_FILL;
        LLColor4 border = style::ADD_BORDER;
        if (mPressed)
        {
            fill = pressed(fill);
        }
        else if (mHover && getEnabled())
        {
            fill = hovered(fill);
            border = style::BUTTON_BORDER_HOVER;
        }
        drawRoundBordered(r, style::CARD_RADIUS, Fill::solid(faded(fill, a)), faded(border, a));
        const F32 h = r.getHeight();
        drawIcon(mIcon.get(), r.getCenterX(), h * 0.58f, style::ADD_ICON, faded(style::ICON_LIGHT, a));
        drawText(mFont, mLabelW, r.getCenterX(), h * 0.30f, faded(style::TEXT_WHITE, a), LLFontGL::HCENTER,
                 LLFontGL::VCENTER, S32(r.getWidth()) - 8, true);
        drawSearchHighlight(r);
        drawFocusRing(r, style::CARD_RADIUS);
    }

    // =========================================================================================
    // PsSegment

    PsSegment::Params::Params()
    :   left_label("left_label"),
        right_label("right_label"),
        labels("labels")
    {
        tab_stop(true);
    }

    PsSegment::PsSegment(const Params& p)
    :   PsCtrl(p),
        mOwnTip(p.tool_tip()),
        mFont(font(style::FONT_BUTTON))
    {
        std::vector<std::string> labels;
        if (!p.labels().empty())
        {
            LLStringUtil::getTokens(p.labels(), labels, "|");
        }
        else
        {
            labels = { p.left_label(), p.right_label() };
        }
        setChoices(labels);
        if (p.initial_value.isProvided())
        {
            setValue(p.initial_value());
        }
    }

    void PsSegment::setChoices(const std::vector<std::string>& labels)
    {
        mLabels.clear();
        for (const std::string& label : labels)
        {
            mLabels.push_back(utf8str_to_wstring(label));
        }
        while (mLabels.size() < 2)
        {
            mLabels.emplace_back();
        }
        mIndex = llmin(mIndex, getChoiceCount() - 1);
        mHoverIndex = -1;
    }

    void PsSegment::setChoiceToolTips(const std::vector<std::string>& tips)
    {
        mTips = tips;
        showHoverTip();
    }

    void PsSegment::setValue(const LLSD& value)
    {
        mIndex = llclamp((S32)value.asInteger(), -1, getChoiceCount() - 1);
    }

    F32 PsSegment::edge(S32 index) const
    {
        return look::segmentEdge(localRectf(), index, getChoiceCount());
    }

    S32 PsSegment::indexAt(S32 x) const
    {
        const S32 count = getChoiceCount();
        for (S32 i = 0; i + 1 < count; ++i)
        {
            if (F32(x) < edge(i + 1))
            {
                return i;
            }
        }
        return count - 1;
    }

    void PsSegment::showHoverTip()
    {
        if (mTips.empty())
        {
            return;
        }
        const bool own = mHoverIndex < 0 || mHoverIndex >= (S32)mTips.size() || mTips[mHoverIndex].empty();
        setToolTip(own ? mOwnTip : mTips[mHoverIndex]);
    }

    bool PsSegment::handleHover(S32 x, S32 y, MASK mask)
    {
        const S32 index = indexAt(x);
        if (index != mHoverIndex)
        {
            mHoverIndex = index;
            showHoverTip();
        }
        return PsCtrl::handleHover(x, y, mask);
    }

    void PsSegment::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHoverIndex = -1;
        showHoverTip();
        PsCtrl::onMouseLeave(x, y, mask);
    }

    void PsSegment::choose(S32 index)
    {
        index = llclamp(index, 0, getChoiceCount() - 1);
        if (index != mIndex)
        {
            mIndex = index;
            commitChange();
        }
    }

    void PsSegment::onClick(S32 x, S32)
    {
        // the keyboard steps to the next one (two choices: flips), round to the first after the last
        choose(x < 0 ? (mIndex + 1) % getChoiceCount() : indexAt(x));
    }

    bool PsSegment::handleKeyHere(KEY key, MASK mask)
    {
        // three or more: as a radio group (two keep the arrows for what is around them, as before)
        if (getEnabled() && mask == MASK_NONE && getChoiceCount() > 2)
        {
            switch (key)
            {
                case KEY_LEFT:  choose(llmax(0, mIndex - 1)); return true;
                case KEY_RIGHT: choose(mIndex + 1);           return true;
                case KEY_HOME:  choose(0);                    return true;
                case KEY_END:   choose(getChoiceCount() - 1); return true;
                default: break;
            }
        }
        return PsCtrl::handleKeyHere(key, mask);
    }

    void PsSegment::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        const S32 count = getChoiceCount();
        const bool hover = mHover && getEnabled();
        look::segmentBar(r, count, mIndex, a);
        for (S32 i = 0; i < count; ++i)
        {
            look::segmentLabel(r, i, count, i == mIndex, hover && i == mHoverIndex, mFont, mLabels[i], a);
        }
        drawSearchHighlight(r);
        drawFocusRing(r, style::SEG_RADIUS);
    }

    // =========================================================================================
    // PsShutter

    PsShutter::Params::Params()
    {
        tab_stop(true);
    }

    PsShutter::PsShutter(const Params& p)
    :   PsCtrl(p)
    {
    }

    void PsShutter::flash()
    {
        mBusy.reset();
        mBusyShown = true;
    }

    void PsShutter::onClick(S32, S32)
    {
        flash();
        commitChange();
    }

    void PsShutter::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        const F32 cx = r.getCenterX();
        const F32 cy = r.getCenterY();
        const F32 radius = llmin(r.getWidth(), r.getHeight()) * 0.5f - 2.f;      // room for the shadow
        if (mBusyShown && mBusy.getElapsedTimeF32() > style::SHUTTER_BUSY_SECONDS)
        {
            mBusyShown = false;
        }
        drawShadow(cx, cy, radius, 0.35f * a);
        drawCircle(cx, cy, radius, Fill::solid(faded(style::SHUTTER_RING, a)));
        drawCircle(cx, cy, radius - style::SHUTTER_RING_WIDTH, Fill::solid(faded(style::PANEL_MID, a)));
        F32 inner = radius - style::SHUTTER_RING_WIDTH - style::SHUTTER_GROOVE;
        if (mPressed)
        {
            inner *= style::SHUTTER_PRESSED_SCALE;
        }
        Fill fill = Fill::vertical(style::SHUTTER_TOP, style::SHUTTER_BOTTOM);
        if (mHover && getEnabled() && !mPressed)
        {
            fill = fill.hovered();
        }
        drawCircle(cx, cy, inner, fill.faded(a * (mBusyShown ? 0.5f : 1.f)));
        drawFocusRing(LLRectf(cx - radius, cy + radius, cx + radius, cy - radius), radius);
    }

    // =========================================================================================
    // PsButton

    PsButton::Params::Params()
    {
        tab_stop(true);
    }

    PsButton::PsButton(const Params& p)
    :   PsCtrl(p),
        mFont(font(style::FONT_BUTTON))
    {
    }

    void PsButton::draw()
    {
        look::button(localRectf(), mPressed, mHover && getEnabled(), false, drawAlpha(), mFont, mLabelW, nullptr, 0, *this);
        drawSearchHighlight(localRectf());
    }

    // =========================================================================================
    // PsTab

    PsTab::Params::Params()
    :   auto_width("auto_width", true)
    {
        tab_stop(true);
    }

    PsTab::PsTab(const Params& p)
    :   PsCtrl(p),
        mAutoWidth(p.auto_width),
        mFont(font(style::FONT_BODY))
    {
        if (p.initial_value.isProvided())
        {
            mSelected = p.initial_value().asBoolean();
        }
        fitWidth();
    }

    void PsTab::onLabelChanged()
    {
        fitWidth();
    }

    void PsTab::fitWidth()
    {
        if (!mAutoWidth || !mFont)
        {
            return;
        }
        const S32 w = mFont->getWidth(mLabelW.c_str()) + 2 * style::TAB_PAD;
        if (w != getRect().getWidth())
        {
            reshape(w, getRect().getHeight());
            rearrangeParent(this);
        }
    }

    void PsTab::onClick(S32, S32)
    {
        takeFocusFromSibling();
        if (!mSelected)
        {
            mSelected = true;
            clearSiblingValues();
            commitChange();
        }
    }

    void PsTab::draw()
    {
        const LLRectf r = localRectf();
        drawUnderlineTab(mFont, mLabelW, r, mSelected, mHover && getEnabled(), drawAlpha());
        drawSearchHighlight(r);
        drawFocusRing(r, 4.f);
    }

    // =========================================================================================
    // PsListRow

    PsListRow::Params::Params()
    :   starrable("starrable", true)
    {
        tab_stop(true);
    }

    PsListRow::PsListRow(const Params& p)
    :   PsCtrl(p),
        mStarrable(p.starrable),
        mFont(font(style::FONT_BODY)),
        mStar(icon("star")),
        mStarFilled(icon("star_filled"))
    {
        if (p.initial_value.isProvided())
        {
            mChosen = p.initial_value().asBoolean();
        }
    }

    LLRectf PsListRow::starRect() const
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        return LLRectf(w - F32(style::LIST_STAR_HIT), h, w, 0.f);
    }

    bool PsListRow::handleHover(S32 x, S32 y, MASK mask)
    {
        mStarHover = mStarrable && starRect().pointInRect(F32(x), F32(y));
        return PsCtrl::handleHover(x, y, mask);
    }

    bool PsListRow::handleKeyHere(KEY key, MASK mask)
    {
        if (getEnabled() && mask == MASK_SHIFT && (key == KEY_RETURN || key == ' ') && mStarrable && mOnStar)
        {
            // one press, one star (a held key's repeats would light it and put it out, and reload the list each time)
            if (!(gKeyboard && gKeyboard->getKeyRepeated(key)))
            {
                mOnStar(this);
            }
            return true;
        }
        return PsCtrl::handleKeyHere(key, mask);
    }

    void PsListRow::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mStarHover = false;
        PsCtrl::onMouseLeave(x, y, mask);
    }

    void PsListRow::setBadge(S32 count, bool quiet)
    {
        count = llmax(0, count);
        mBadgeQuiet = quiet;
        if (count != mBadge)
        {
            mBadge = count;
            mBadgeText = badgeText(count);
        }
    }

    void PsListRow::onClick(S32 x, S32 y)
    {
        if (mStarrable && mOnStar && x >= 0 && starRect().pointInRect(F32(x), F32(y)))
        {
            mOnStar(this);
            return;
        }
        if (!mMissing)
        {
            commitChange();
        }
    }

    void PsListRow::draw()
    {
        const LLRectf r = localRectf();
        const F32 a = drawAlpha();
        const F32 h = r.getHeight();
        const bool hover = mHover && getEnabled();
        if (mChosen)
        {
            drawRound(r, style::LIST_ROW_RADIUS, Fill::solid(faded(style::LIST_SELECTED, a)));
            fillRect(LLRectf(0.f, h - 6.f, style::LIST_ROW_BAR, 6.f), Fill::solid(faded(style::ACCENT_LINE, a)));
        }
        else if (hover && !mMissing)
        {
            drawRound(r, style::LIST_ROW_RADIUS, Fill::solid(faded(mPressed ? style::RAIL_PRESS : style::RAIL_HOVER, a)));
        }

        const F32 x = F32(style::LIST_ROW_PAD);
        const bool badge = !mStarrable && mBadge > 0;
        const F32 right_room = mStarrable ? F32(style::LIST_STAR_HIT) : (badge ? F32(style::LIST_STAR_HIT) + x : x);
        const S32 room = llmax(0, S32(r.getWidth() - x - right_room));
        if (badge)
        {
            drawBadge(mBadgeText, r.getWidth() - x - F32(style::LIST_STAR_HIT) * 0.5f, h * 0.5f, mBadgeQuiet, a);
        }
        const LLColor4& text = mMissing ? style::TEXT_DIM : (mChosen ? style::TEXT_WHITE : style::LIST_ROW_TEXT);
        drawText(mFont, mLabelW, x, h * 0.5f, faded(text, a), LLFontGL::LEFT, LLFontGL::VCENTER, room, true);

        if (mStarrable)
        {
            const LLRectf s = starRect();
            const LLColor4& color = mStarred ? style::LIST_STAR_ON : (mStarHover ? style::ICON_LIGHT : style::LIST_STAR_OFF);
            drawIcon((mStarred ? mStarFilled : mStar).get(), s.getCenterX(), h * 0.5f, style::LIST_STAR, faded(color, a));
        }
        drawSearchHighlight(r);
        drawFocusRing(r, style::LIST_ROW_RADIUS);
    }
}
