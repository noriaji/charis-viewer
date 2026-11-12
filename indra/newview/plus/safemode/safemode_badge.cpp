/**
 * @file safemode_badge.cpp
 * @brief Safe mode (research/254): the small "Safe mode" box at the bottom
 *        right of the screen while a safe start runs.
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

// Charis's own. The see-through box of the world's chips (research/221 part 8,
// plus/photoscreen/psui) with two lines: "Safe mode" in white and what it does
// in the second lines' grey. Only values of psstyle.h; the texts are in
// panel_plus_safemode.xml (en, ja). It tells, and takes no mouse: how to leave
// safe mode is in the Charis menu ("start in safe mode next time too") and in
// research/254.
//
// Where: at the bottom right of the area floaters snap to (between the
// toolbars), as the shader reload notice sits at the bottom in the middle
// (plus/shaderreload). In the world it is at the back of the window's root
// view: over the 3D picture, under every floater, toolbar, menu and toast (a
// toast at the bottom right covers it for a while). At the login screen the
// login panel would cover that, so until the world is shown (STATE_STARTED)
// it is at the front of the root view instead; the startup state's event
// moves it, once.
//
// Cost: a normal start makes no view. A safe start draws one box and two
// lines a frame; the texts and their widths are made once (again only when
// the font changes). Not in a snapshot.

#include "llviewerprecompiledheaders.h"

#include "safemode_badge.h"

#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"

#include "llevents.h"
#include "llstartup.h"
#include "lluictrlfactory.h"
#include "llview.h"

extern bool gSnapshot;

namespace
{
    constinit plus::psui::StringsPanel sUIStrings("panel_plus_safemode.xml");
}

std::string plus::safemode::uiText(const char* name, const LLStringUtil::format_map_t& args)
{
    return sUIStrings.get(name, args);
}

namespace plus::safemode::badge
{
    namespace
    {
        namespace psui  = plus::psui;
        namespace style = plus::psui::style;

        constexpr char LISTENER[]     = "PlusSafeModeBadge";

        // research/221 part 8's box with two lines of part 24's height (the shader reload notice).
        constexpr S32 HEIGHT = 2 * style::RELOAD_PAD_Y + 2 * style::RELOAD_TEXT_LINE;


        class BadgeView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit BadgeView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void setTexts(const LLWString& title, const LLWString& detail)
            {
                mTitle = title;
                mDetail = detail;
                mTitleFont = nullptr;   // measured again at the next draw
            }

            void findSnapRegion(LLView* root)
            {
                if (LLView* view = psui::findSnapRegion(root))
                {
                    mSnapRegion = view->getHandle();
                }
            }

            void draw() override
            {
                if (gSnapshot || plus::presets::snapshotInProgress())
                {
                    return;
                }
                const LLFontGL* title_font = psui::font(style::FONT_BODY);
                const LLFontGL* detail_font = psui::font(style::FONT_SMALL);
                if (!title_font || !detail_font)
                {
                    return;
                }
                if (title_font != mTitleFont || detail_font != mDetailFont)
                {
                    mTitleFont = title_font;
                    mDetailFont = detail_font;
                    mTextWidth = llmax(title_font->getWidth(mTitle.c_str()), detail_font->getWidth(mDetail.c_str()));
                }

                const S32 width = 2 * style::CHIP_PAD + mTextWidth;
                const LLRect room = psui::worldChipArea(*this, mSnapRegion.get());
                if (room.getWidth() < width + 2 * style::RELOAD_MARGIN || room.getHeight() < HEIGHT + 2 * style::RELOAD_MARGIN)
                {
                    return;
                }
                const F32 right = (F32)(room.mRight - style::RELOAD_MARGIN);
                const F32 left = right - (F32)width;
                const F32 bottom = (F32)(room.mBottom + style::RELOAD_MARGIN);
                const F32 top = bottom + (F32)HEIGHT;
                psui::drawRound(LLRectf(left, top, right, bottom), style::CHIP_RADIUS, psui::Fill::solid(style::CHIP_FILL));

                const F32 text_x = left + (F32)style::CHIP_PAD;
                const F32 line_half = style::RELOAD_TEXT_LINE * 0.5f;
                psui::drawText(title_font, mTitle, text_x, top - (F32)style::RELOAD_PAD_Y - line_half, style::TEXT_WHITE,
                               LLFontGL::LEFT, LLFontGL::VCENTER);
                psui::drawText(detail_font, mDetail, text_x, bottom + (F32)style::RELOAD_PAD_Y + line_half, style::TEXT_SUB,
                               LLFontGL::LEFT, LLFontGL::VCENTER);
            }

        private:
            LLHandle<LLView> mSnapRegion;
            LLWString        mTitle;
            LLWString        mDetail;
            const LLFontGL*  mTitleFont = nullptr;     // mTextWidth is for these fonts
            const LLFontGL*  mDetailFont = nullptr;
            S32              mTextWidth = 0;
        };

        LLHandle<LLView> sView;

        // Under the floaters once the world is shown (the login panel is gone then).
        void sendToBack()
        {
            LLView* badge = sView.get();
            LLView* root = psui::overlayRoot();
            if (badge && root && badge->getParent() == root)
            {
                root->sendChildToBack(badge);
            }
        }

        bool onStartupState(const LLSD& info)
        {
            if (info["enum"].asInteger() == STATE_STARTED)
            {
                sendToBack();   // STATE_STARTED comes once; the listener stays (not taken off while it is called)
            }
            return false;
        }
    }

    S32 cornerHeight()
    {
        const LLView* badge = sView.get();
        return badge && badge->getVisible() ? HEIGHT + style::RELOAD_MARGIN : 0;
    }

    void attach()
    {
        // at the front: over the login panel
        if (!psui::attachWorldOverlay<BadgeView>(sView, "plus_safe_mode_badge", true, psui::OverlayOrder::Front,
                                                 [](BadgeView& badge, LLView* root) {
                                                     badge.setTexts(utf8str_to_wstring(uiText("badge_title")),
                                                                    utf8str_to_wstring(uiText("badge_detail")));
                                                     badge.findSnapRegion(root);
                                                 }))
        {
            return;
        }
        if (LLStartUp::getStartupState() >= STATE_STARTED)
        {
            sendToBack();
        }
        else
        {
            LLEventPumps::instance().obtain("StartupState").listen(LISTENER, &onStartupState);
        }
    }
}
