/**
 * @file reloadnotice.cpp
 * @brief research/250: the notice at the bottom of the world while the
 *        shaders are made in the background ("Preparing shaders... 37 %").
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

// Charis's own (the official viewer stands still while it reloads, with a
// wait cursor). A chip over the world in the photo screen's style (research/221
// part 8, plus/photoscreen/psui): the see-through box, the icon, the text in
// white and the share in the second lines' grey, under it a track as the
// sliders' with the accent blue for what is done. Only values of psstyle.h.
//
// Where: at the bottom of the world, in the middle, within the area floaters
// snap to (between the toolbars), at the back of the window's root view as the
// FPS corner (plus/fpscorner): over the 3D picture, under every floater, menu
// and toast, taking no mouse. The preferences' floaters sit in the middle of
// the screen; the bottom is the place they leave free.
//
// Cost: hidden (not drawn) while there is no reload; the text and its width are
// made again only when the kind or the whole per cent changes.

#include "llviewerprecompiledheaders.h"

#include "reloadnotice.h"

#include "plus.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"

#include "llstartup.h"
#include "lluictrlfactory.h"
#include "llview.h"

extern bool gSnapshot;

namespace plus::shaderreload::notice
{
    namespace
    {
        namespace psui  = plus::psui;
        namespace style = plus::psui::style;

        constexpr char ICON[]         = "quality";

        // research/221 part 8 (the chip), with a track under the text while the build goes on (part 24).
        constexpr F32 BAR_RADIUS = style::SLIDER_TRACK * 0.5f;
        constexpr S32 HEIGHT_WITH_BAR = 2 * style::RELOAD_PAD_Y + style::RELOAD_TEXT_LINE + style::RELOAD_BAR_GAP +
                                        (S32)style::SLIDER_TRACK;

        constinit psui::StringsPanel sStrings("panel_plus_shaderreload.xml");    // the texts (localized)

        std::string text(const std::string& name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
        {
            return sStrings.get(name, args);
        }

        // "37%" (the language's way).
        LLWString percentText(S32 percent)
        {
            LLStringUtil::format_map_t args;
            args["[PERCENT]"] = llformat("%d", percent);
            return utf8str_to_wstring(text("percent", args));
        }

        class NoticeView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit NoticeView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void findSnapRegion(LLView* root)
            {
                if (LLView* view = psui::findSnapRegion(root))
                {
                    mSnapRegion = view->getHandle();
                }
            }

            // The texts again only when what they say changed.
            void set(Kind kind, F32 fraction)
            {
                mFraction = llclamp(fraction, 0.f, 1.f);
                const S32 percent = kind == Kind::Failed ? -1 : ll_round(mFraction * 100.f);
                if (kind == mKind && percent == mPercent && mHaveText)
                {
                    return;
                }
                const bool new_kind = kind != mKind || !mHaveText;
                mKind = kind;
                mPercent = percent;
                mHaveText = true;
                if (new_kind)
                {
                    mText = utf8str_to_wstring(text(kind == Kind::Preparing ? "preparing" : kind == Kind::Switching ? "switching" : "failed"));
                }
                if (percent >= 0)
                {
                    mPercentText = percentText(percent);
                }
                else
                {
                    mPercentText.clear();
                }
                mFont = nullptr;    // measured again at the next draw
            }

            void draw() override
            {
                if (gSnapshot || plus::presets::snapshotInProgress() || LLStartUp::getStartupState() < STATE_STARTED)
                {
                    return;
                }
                const LLFontGL* font = psui::font(style::FONT_BODY);
                if (!font)
                {
                    return;
                }
                if (font != mFont)
                {
                    measure(font);
                }
                if (!mIcon)
                {
                    mIcon = psui::icon(ICON);
                }

                const bool bar = mKind != Kind::Failed;
                const S32 percent_room = mPercentText.empty() ? 0 : style::CHIP_GAP + mPercentWidth;
                const S32 width = 2 * style::CHIP_PAD + style::CHIP_ICON + style::CHIP_GAP + mTextWidth + percent_room;
                const S32 height = bar ? HEIGHT_WITH_BAR : style::CHIP_HEIGHT;
                const LLRect room = psui::worldChipArea(*this, mSnapRegion.get());
                if (room.getWidth() < width || room.getHeight() < height + style::RELOAD_MARGIN)
                {
                    return;
                }
                const F32 left = (F32)(room.getCenterX() - width / 2);
                const F32 right = left + (F32)width;
                const F32 bottom = (F32)(room.mBottom + style::RELOAD_MARGIN);
                const F32 top = bottom + (F32)height;
                psui::drawRound(LLRectf(left, top, right, bottom), style::CHIP_RADIUS, psui::Fill::solid(style::CHIP_FILL));

                // The text's line: at the top above the track, else in the middle.
                const F32 line_y = bar ? top - (F32)(style::RELOAD_PAD_Y + style::RELOAD_TEXT_LINE / 2) : (top + bottom) * 0.5f;
                const F32 text_x = left + (F32)(style::CHIP_PAD + style::CHIP_ICON + style::CHIP_GAP);
                psui::drawIcon(mIcon.get(), left + (F32)style::CHIP_PAD + style::CHIP_ICON * 0.5f, line_y, style::CHIP_ICON,
                               style::CHIP_ICON_COLOR);
                psui::drawText(font, mText, text_x, line_y, style::TEXT_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
                if (!mPercentText.empty())
                {
                    psui::drawText(font, mPercentText, right - (F32)style::CHIP_PAD, line_y, style::TEXT_SUB, LLFontGL::RIGHT,
                                   LLFontGL::VCENTER);
                }
                if (bar)
                {
                    const F32 track_left = left + (F32)style::CHIP_PAD;
                    const F32 track_right = right - (F32)style::CHIP_PAD;
                    const F32 track_bottom = bottom + (F32)style::RELOAD_PAD_Y;
                    const F32 track_top = track_bottom + style::SLIDER_TRACK;
                    psui::drawRound(LLRectf(track_left, track_top, track_right, track_bottom), BAR_RADIUS,
                                    psui::Fill::solid(style::SLIDER_EMPTY));
                    const F32 done = (track_right - track_left) * mFraction;
                    if (done >= 1.f)
                    {
                        psui::drawRound(LLRectf(track_left, track_top, track_left + done, track_bottom), BAR_RADIUS,
                                        psui::Fill::solid(style::ACCENT));
                    }
                }
            }

        private:
            void measure(const LLFontGL* font)
            {
                mFont = font;
                mTextWidth = font->getWidth(mText.c_str());
                // As wide at 7 % as at 100 %: the box does not jump with each per cent.
                mPercentWidth = mPercentText.empty() ? 0 : font->getWidth(percentText(100).c_str());
            }

            LLHandle<LLView>     mSnapRegion;
            LLPointer<LLUIImage> mIcon;
            LLWString            mText;
            LLWString            mPercentText;
            const LLFontGL*      mFont = nullptr;   // the widths are for this font
            S32                  mTextWidth = 0;
            S32                  mPercentWidth = 0;
            Kind                 mKind = Kind::Preparing;
            S32                  mPercent = -1;
            F32                  mFraction = 0.f;
            bool                 mHaveText = false;
        };

        LLHandle<LLView> sView;

        NoticeView* view()
        {
            return static_cast<NoticeView*>(sView.get());
        }

        // Made once the UI is there (at login), hidden: not while a settings
        // change's event is being handled (a request comes from one).
        void attach()
        {
            psui::attachWorldOverlay<NoticeView>(sView, "plus_shader_reload_notice", false, psui::OverlayOrder::Back,
                                                 [](NoticeView& notice, LLView* root) { notice.findSnapRegion(root); });
        }

        void init()
        {
        }

        plus::FeatureRegistration sRegistration({ "shaderreload", &init, &attach });
    }

    void show(Kind kind, F32 fraction)
    {
        if (NoticeView* notice = view())
        {
            notice->set(kind, fraction);
            if (!notice->getVisible())
            {
                notice->setVisible(true);
            }
        }
    }

    void hide()
    {
        if (LLView* notice = sView.get(); notice && notice->getVisible())
        {
            notice->setVisible(false);
        }
    }
}
