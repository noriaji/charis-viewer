/**
 * @file focushint.cpp
 * @brief The hint over the world while the focus is picked by clicking (research/264).
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

// Esc ends the picking, and that is hard to find (a request of 2026-11-02): while the picking is on, a chip
// at the top of the world, in the middle, says what a click does and how to stop. Before the first click "Click the
// place to focus on", after it "Click to move the focus"; on the right, in the second lines' grey, "Esc to stop".
//
// The look is the photo screen's chip over the world (research/221 part 8, plus/photoscreen/psui; as the shader
// reload's notice, plus/shaderreload/reloadnotice.cpp): the see-through box, the icon, the text in white. Only values
// of psstyle.h. Where: a view of the window's root view at its back (over the 3D picture, under every floater, menu and
// toast), taking no mouse, so the clicks under it still go to the world (and to the picking). It is drawn with the
// UI: with the UI hidden (Ctrl+Alt+F1) there is no hint, the picking itself still works. In the photo screen it goes
// under its parts at the top of the world (the detailed mode's chips and histogram; photoscreen::worldTopTaken()):
// they were under the box at first (focus-hint2).
//
// Cost: made at the first picking, hidden (not drawn) while not picking. A frame while shown: two bool reads; the
// texts and their widths are made again only when "before / after the first click" or the font changes.

#include "llviewerprecompiledheaders.h"

#include "focushint.h"
#include "focuslock.h"

#include "plus/photoscreen/photoscreen.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"

#include "llagentcamera.h"
#include "llstartup.h"
#include "lluictrlfactory.h"
#include "llview.h"

extern bool gSnapshot;

namespace plus::focuslock::hint
{
    namespace
    {
        namespace psui  = plus::psui;
        namespace style = plus::psui::style;

        constexpr char ICON[]         = "focus";

        constinit psui::StringsPanel sStrings("panel_plus_focushint.xml");   // the texts (localized)

        std::string text(const char* name)
        {
            return sStrings.get(name);
        }

        class HintView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit HintView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void findSnapRegion(LLView* root)
            {
                if (LLView* view = psui::findSnapRegion(root))
                {
                    mSnapRegion = view->getHandle();
                }
            }

            void draw() override
            {
                if (gSnapshot || plus::presets::snapshotInProgress() || LLStartUp::getStartupState() < STATE_STARTED ||
                    !isPicking() || gAgentCamera.cameraMouselook())
                {
                    return;
                }
                const LLFontGL* font = psui::font(style::FONT_BUTTON);
                if (!font)
                {
                    return;
                }
                const int placed = hasClickPoint() ? 1 : 0;
                if (placed != mPlaced || font != mFont)
                {
                    setTexts(placed, font);
                }
                if (!mIcon)
                {
                    mIcon = psui::icon(ICON);
                }

                const S32 width = 2 * style::CHIP_PAD + style::CHIP_ICON + 2 * style::CHIP_GAP + mTextWidth + mKeyWidth;
                const S32 height = style::CHIP_HEIGHT;
                const LLRect room = psui::worldChipArea(*this, mSnapRegion.get());
                const S32 box_left = room.getCenterX() - width / 2;
                const S32 box_top  = topOfBox(room, box_left, box_left + width);
                if (room.getWidth() < width || box_top - height < room.mBottom)
                {
                    return;     // a world too small for it
                }
                const F32 left   = (F32)box_left;
                const F32 right  = left + (F32)width;
                const F32 top    = (F32)box_top;
                const F32 bottom = top - (F32)height;
                const F32 line_y = (top + bottom) * 0.5f;
                psui::drawRound(LLRectf(left, top, right, bottom), style::CHIP_RADIUS, psui::Fill::solid(style::CHIP_FILL));
                psui::drawIcon(mIcon.get(), left + (F32)style::CHIP_PAD + style::CHIP_ICON * 0.5f, line_y, style::CHIP_ICON,
                               style::CHIP_ICON_COLOR);
                const F32 text_x = left + (F32)(style::CHIP_PAD + style::CHIP_ICON + style::CHIP_GAP);
                psui::drawText(font, mText, text_x, line_y, style::TEXT_WHITE, LLFontGL::LEFT, LLFontGL::VCENTER);
                psui::drawText(font, mKey, right - (F32)style::CHIP_PAD, line_y, style::TEXT_SUB, LLFontGL::RIGHT,
                               LLFontGL::VCENTER);
            }

        private:
            // The box's top (this view's coordinates) for a box from left to right: FOCUS_HINT_MARGIN under the top
            // of room (the world between the toolbars); in the photo screen also FOCUS_HINT_GAP under its own parts
            // at the top of the world that are over that width (the detailed mode's chips and histogram), so it goes
            // down past them instead of over them. A few integer sums a frame; it follows the photo screen's layout
            // (the mode, the window's size) as it is.
            S32 topOfBox(const LLRect& room, S32 left, S32 right) const
            {
                S32 top = room.mTop - style::FOCUS_HINT_MARGIN;
                LLRect world;
                if (plus::photoscreen::shownWorldRect(world))
                {
                    S32 screen_left = 0, screen_right = 0, screen_y = 0;
                    localPointToScreen(left, 0, &screen_left, &screen_y);
                    localPointToScreen(right, 0, &screen_right, &screen_y);
                    const S32 taken = plus::photoscreen::worldTopTaken(screen_left, screen_right);
                    if (taken > 0)
                    {
                        S32 local_x = 0, under = 0;
                        screenPointToLocal(0, world.mTop - taken - style::FOCUS_HINT_GAP, &local_x, &under);
                        top = llmin(top, under);
                    }
                }
                return top;
            }

            void setTexts(int placed, const LLFontGL* font)
            {
                mPlaced    = placed;
                mFont      = font;
                mText      = utf8str_to_wstring(text(placed ? "moved" : "first"));
                mKey       = utf8str_to_wstring(text("esc"));
                mTextWidth = font->getWidth(mText.c_str());
                mKeyWidth  = font->getWidth(mKey.c_str());
            }

            LLHandle<LLView>     mSnapRegion;
            LLPointer<LLUIImage> mIcon;
            LLWString            mText;     // what a click does
            LLWString            mKey;      // how to stop
            const LLFontGL*      mFont = nullptr;   // the widths are for this font
            S32                  mTextWidth = 0;
            S32                  mKeyWidth = 0;
            int                  mPlaced = -1;      // the texts are for: 0 before the first click, 1 after; -1 none yet
        };

        LLHandle<LLView> sView;

        // Made at the first picking, once the UI is there.
        LLView* ensureView()
        {
            if (LLView* existing = sView.get())
            {
                return existing;
            }
            return psui::attachWorldOverlay<HintView>(sView, "plus_focus_hint", false, psui::OverlayOrder::Back,
                                                      [](HintView& hint, LLView* root) { hint.findSnapRegion(root); });
        }
    }

    void setShown(bool shown)
    {
        LLView* hint = shown ? ensureView() : sView.get();
        if (hint && hint->getVisible() != shown)
        {
            hint->setVisible(shown);
        }
    }
}
