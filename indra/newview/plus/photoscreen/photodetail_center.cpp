/**
 * @file photodetail_center.cpp
 * @brief The middle of the detailed photo screen: the chips, the read-out, the chat input and the icons
 *        (moved out of photodetail.cpp), drawn as the target picture (research/221 8.-12.).
 *
 * The chips are drop-downs drawn as the picture's chips (ChipCombo: the official list opens under them; the
 * closed box is the see-through rounded chip with its icon, text and chevron). Each chip's width follows its
 * text; it is fitted in the tick when the chosen row changed, not while drawing. The read-out, the icons and
 * the histogram's frame are the photo screen's widgets (psui/pswidgets.h) in panel_plus_photoscreen_hud.xml.
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

#include "photodetail_center.h"
#include "photodetail.h"

#include "photodetail_hud.h"
#include "photodetail_layout.h"
#include "photodetail_left.h"
#include "photodetail_right.h"
#include "photodetail_rows.h"
#include "photodetail_state.h"
#include "photoscreen.h"
#include "plus/flycammouse/flycammouse.h"
#include "plus/photomode/photolook.h"
#include "plus/photomode/photomode.h"
#include "plus/photomode/photoshot.h"
#include "psui/psstyle.h"
#include "psui/pswidgets.h"

#include <cmath>
#include <initializer_list>

#include "llcombobox.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

namespace plus::photoscreen::detail
{
    using namespace plus::photoscreen::sizes;

    namespace
    {
        // For the eye (research/218): the shutter speed stays 1/125 and the ISO follows the exposure
        // (RenderExposure 1.0 is ISO 100, a stop up is 200), to the nearest of the usual steps.
        float nearestIso(float iso)
        {
            static const float steps[] = { 50.f, 64.f, 80.f, 100.f, 125.f, 160.f, 200.f, 250.f, 320.f, 400.f };
            float best = steps[0];
            for (float step : steps)
            {
                if (fabsf(logf(step / iso)) < fabsf(logf(best / iso)))
                {
                    best = step;
                }
            }
            return best;
        }

        // The read-out's four items ("F 1.8", "1/125", "ISO 100", "50 mm"; the same in every language), written
        // only when a number changed.
        struct ReadoutShown
        {
            S32 f_tenths = -1, iso = -1, mm = -1;
        };
        ReadoutShown sReadout;

        // The flycam's icon and the line over the icons (photo-flycam), as last written (-1: not yet): on, and the
        // way the mouse looks (PlusFlycamMouseMode), whose keys the icon's tip says.
        int sFlycamShown = -1;
        // The focus icon's tip as last written (-1: not yet): 1 for the wide layout (the right column's lens section).
        int sFocusTipWide = -1;
        // The flycam line's box as the XML has it: its width is the least, the text's pad at each side.
        S32 sBadgeMinW = 0, sBadgePad = 0;

        // The flycam line's box as wide as its text (a long key bound, "Ctrl+Alt+Shift+PgDn"), at least as the XML has
        // it, its right edge where it was (the chat's width follows its left edge, layoutHud).
        void fitBadge(LLView& badge, psui::PsLabel& text)
        {
            const LLFontGL* font = text.getFont();
            if (!font || sBadgeMinW <= 0)
            {
                return;
            }
            const LLRect r = badge.getRect();
            const S32 room = llmax(sBadgeMinW, sS.world.getWidth() - 2 * WORLD_INSET);
            const S32 want = llclamp(ll_round(font->getWidthF32(text.getLabelText())) + 2 * sBadgePad + 2, sBadgeMinW, room);
            if (want != r.getWidth())
            {
                badge.setShape(LLRect(r.mRight - want, r.mTop, r.mRight, r.mBottom));
                const LLRect t = text.getRect();
                text.setShape(LLRect(sBadgePad, t.mTop, want - sBadgePad, t.mBottom));
            }
        }

        void refreshFlycam(LLPanel* hud)
        {
            static LLCachedControl<S32> look(gSavedSettings, "PlusFlycamMouseMode", 1);
            const bool on    = plus::flycammouse::isOn();
            const S32  mode  = llclamp((S32)look, 0, 2);
            const int  shown = (on ? 1 : 0) + 2 * mode;
            if (shown == sFlycamShown)
            {
                return;
            }
            sFlycamShown = shown;
            // the keys as bound now (read only when this changes; a key bound anew shows the next time)
            LLStringUtil::format_map_t args;
            args["[KEY]"] = on ? plus::flycammouse::leaveKey() : plus::flycammouse::switchKey();
            if (LLUICtrl* button = hud->findChild<LLUICtrl>("b_flycam"))
            {
                button->setValue(on);
                button->setToolTip(hud->getString("FlycamTip" + std::to_string(mode), args));
            }
            if (LLView* badge = hud->findChildView("flycam_badge", false))
            {
                if (on)
                {
                    if (psui::PsLabel* text = badge->findChild<psui::PsLabel>("flycam_badge_text"))
                    {
                        text->setLabelText(hud->getString("FlycamBadge", args));
                        fitBadge(*badge, *text);
                        if (badge->getVisible())
                        {
                            layoutHud();    // shown already (the way the mouse looks changed): its left edge may have moved
                        }
                    }
                }
                if (badge->getVisible() != on)
                {
                    badge->setVisible(on);
                    layoutHud();    // the chat makes room for the line, or takes it back
                }
            }
        }

        void refreshReadout(LLPanel* hud)
        {
            static LLCachedControl<F32> f_number(gSavedSettings, "CameraFNumber", 9.f);
            static LLCachedControl<F32> angle(gSavedSettings, "CameraAngle", 1.047f);
            static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
            const S32 f_tenths = ll_round((F32)f_number * 10.f);
            const S32 iso      = ll_round(nearestIso(100.f * llmax((F32)exposure, 0.05f)));
            const S32 mm       = ll_round(angleToMm(angle));
            if (f_tenths == sReadout.f_tenths && iso == sReadout.iso && mm == sReadout.mm)
            {
                return;
            }
            psui::PsReadout* readout = hud->findChild<psui::PsReadout>("readout");
            if (!readout)
            {
                return;
            }
            sReadout = { f_tenths, iso, mm };
            readout->setItem(0, llformat("F %.1f", f_tenths / 10.f));
            readout->setItem(1, "1/125");
            readout->setItem(2, llformat("ISO %d", iso));
            readout->setItem(3, llformat("%d mm", mm));
        }

        // --- the chips -------------------------------------------------------------------------------

        // A drop-down drawn as a chip of the target picture (research/221 8.): the see-through rounded box with an
        // icon, the chosen row's text and, for some, a chevron. The official button inside keeps the clicks and
        // the official list opens under it (its colours: PsCombo's). XUI tag plus_ps_chip_combo (its defaults:
        // widgets/plus_ps_chip_combo.xml).
        class ChipCombo : public psui::PsCombo
        {
        public:
            struct Params : public LLInitParam::Block<Params, psui::PsCombo::Params>
            {
                Optional<std::string> icon;
                Optional<bool>        chevron;
                Params()
                :   icon("icon"),
                    chevron("chevron", false)
                {
                }
            };

            void onMouseEnter(S32 x, S32 y, MASK mask) override
            {
                mHover = true;
                psui::PsCombo::onMouseEnter(x, y, mask);
            }

            void onMouseLeave(S32 x, S32 y, MASK mask) override
            {
                mHover = false;
                psui::PsCombo::onMouseLeave(x, y, mask);
            }

            // A closed drop-down passes the wheel on (PlusComboWheelSelect); over the world that would zoom the
            // world's camera behind the chip. Taken here, as the bottom strip does; an open list still scrolls.
            bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
            {
                psui::PsCombo::handleScrollWheel(x, y, clicks);
                return true;
            }

            // The width for the chosen row's text (from the tick, when the row changed); the row of chips is laid
            // out again.
            void fit()
            {
                const S32 index = getCurrentIndex();
                const S32 count = getItemCount();
                if (index == mFitIndex && count == mFitCount)
                {
                    return;
                }
                mFitIndex = index;
                mFitCount = count;
                namespace st = psui::style;
                const LLWString text = utf8str_to_wstring(getSelectedItemLabel());
                S32 width = st::CHIP_PAD * 2 + (mFont ? mFont->getWidth(text.c_str()) : 0);
                if (mIcon.notNull())
                {
                    width += st::CHIP_ICON + st::CHIP_GAP;
                }
                if (mHasChevron)
                {
                    width += st::CHIP_GAP + st::COMBO_CHEVRON;
                }
                if (width != getRect().getWidth())
                {
                    reshape(width, getRect().getHeight());
                    if (psui::PsRow* row = dynamic_cast<psui::PsRow*>(getParent()))
                    {
                        row->arrange();
                    }
                }
            }

            void draw() override
            {
                namespace st = psui::style;
                // Two integers a frame; the text is made again only when they change.
                const S32 index = getCurrentIndex();
                const S32 count = getItemCount();
                if (index != mShownIndex || count != mShownCount)
                {
                    mShownIndex = index;
                    mShownCount = count;
                    mShown = utf8str_to_wstring(getSelectedItemLabel());
                }
                const bool open = mList && mList->getVisible();
                const F32  w = F32(getRect().getWidth());
                const F32  h = F32(getRect().getHeight());
                const F32  a = getDrawContext().mAlpha * (getEnabled() ? 1.f : 0.4f);
                const LLRectf r(0.f, h, w, 0.f);
                LLColor4 fill = st::CHIP_FILL;
                if (open)
                {
                    fill = psui::pressed(fill);
                }
                else if (mHover && getEnabled())
                {
                    fill = lerp(fill, LLColor4(1.f, 1.f, 1.f, fill.mV[VALPHA]), 0.08f);   // as PsChip
                }
                psui::drawRound(r, st::CHIP_RADIUS, psui::Fill::solid(psui::faded(fill, a)));
                F32 x = F32(st::CHIP_PAD);
                if (mIcon.notNull())
                {
                    psui::drawIcon(mIcon.get(), x + F32(st::CHIP_ICON) * 0.5f, h * 0.5f, st::CHIP_ICON,
                                   psui::faded(st::CHIP_ICON_COLOR, a));
                    x += F32(st::CHIP_ICON + st::CHIP_GAP);
                }
                const F32 right_room = F32(st::CHIP_PAD) + (mHasChevron ? F32(st::CHIP_GAP + st::COMBO_CHEVRON) : 0.f);
                psui::drawText(mFont, mShown, x, h * 0.5f, psui::faded(st::TEXT_WHITE, a), LLFontGL::LEFT, LLFontGL::VCENTER,
                               llmax(0, S32(w - x - right_room)), true);
                if (mHasChevron)
                {
                    psui::drawIcon(mChevron.get(), w - F32(st::CHIP_PAD) - F32(st::COMBO_CHEVRON) * 0.5f, h * 0.5f,
                                   st::COMBO_CHEVRON, psui::faded(st::CHIP_ICON_COLOR, a));
                }
                if (gFocusMgr.childHasKeyboardFocus(this) && !open)
                {
                    psui::drawRing(LLRectf(-2.f, h + 2.f, w + 2.f, -2.f), st::CHIP_RADIUS + 2.f, 1.f,
                                   psui::faded(st::FOCUS_RING, getDrawContext().mAlpha));
                }
                // The official button keeps the clicks (it is not drawn); the open list is drawn as it is.
                if (open)
                {
                    drawChild(mList);
                }
            }

        protected:
            friend class LLUICtrlFactory;
            explicit ChipCombo(const Params& p)
            :   psui::PsCombo(p),
                mIcon(psui::icon(p.icon)),
                mChevron(psui::icon("chevron_down")),
                mFont(psui::font(psui::style::FONT_BUTTON)),
                mHasChevron(p.chevron)
            {
            }

        private:
            LLPointer<LLUIImage> mIcon, mChevron;
            const LLFontGL* mFont;
            bool mHasChevron;
            bool mHover = false;
            LLWString mShown;
            S32 mShownIndex = -2, mShownCount = -1;
            S32 mFitIndex = -2, mFitCount = -1;
        };

        LLDefaultChildRegistry::Register<ChipCombo> sChipComboRegistration("plus_ps_chip_combo");

        constexpr const char* CHIP_NAMES[] = { "chip_look", "chip_res", "chip_aspect", "chip_grid" };

        void fillChips(LLPanel* top)
        {
            LLComboBox* look = top->getChild<LLComboBox>("chip_look");
            look->removeall();
            sS.lookIds.clear();
            S32 index = 0;
            for (const pm::LookChoice& choice : pm::allLooks())
            {
                if (choice.exists)
                {
                    look->add(choice.label.empty() ? str("LookNone") : choice.label, LLSD(index++));
                    sS.lookIds.push_back(choice.id);
                }
            }
            LLComboBox* size = top->getChild<LLComboBox>("chip_res");
            size->removeall();
            for (int i = 0; i < pm::SHOT_SIZE_COUNT; ++i)
            {
                size->add(str("ChipSize" + std::to_string(i)), LLSD(i));
            }
            LLComboBox* aspect = top->getChild<LLComboBox>("chip_aspect");
            aspect->removeall();
            for (int i = 0; i < pm::SHOT_ASPECT_COUNT; ++i)
            {
                aspect->add(str("Aspect" + std::to_string(i)), LLSD(i));
            }
            LLComboBox* grid = top->getChild<LLComboBox>("chip_grid");
            grid->removeall();
            grid->add(str("GridOff"), LLSD(0));
            for (int i = 1; i <= GRID_KINDS; ++i)
            {
                grid->add(str("Grid" + std::to_string(i)), LLSD(i));
            }
        }

        void refreshChips()
        {
            LLPanel* top = sS.top.get();
            if (!top)
            {
                return;
            }
            static LLCachedControl<S32>  size(gSavedSettings, SETTING_SIZE, 0);
            static LLCachedControl<S32>  aspect(gSavedSettings, SETTING_ASPECT, 0);
            static LLCachedControl<bool> guide_on(gSavedSettings, SETTING_GUIDE_ON, false);
            static LLCachedControl<S32>  guide_kind(gSavedSettings, SETTING_GUIDE_KIND, 1);
            setComboIfChanged(top->getChild<LLComboBox>("chip_res"), LLSD((S32)size));
            setComboIfChanged(top->getChild<LLComboBox>("chip_aspect"), LLSD((S32)aspect));
            setComboIfChanged(top->getChild<LLComboBox>("chip_grid"), LLSD(guide_on ? llclamp((S32)guide_kind, 0, GRID_KINDS) : 0));
            const std::string now = pm::currentLook();
            for (size_t i = 0; i < sS.lookIds.size(); ++i)
            {
                if (sS.lookIds[i] == now)
                {
                    setComboIfChanged(top->getChild<LLComboBox>("chip_look"), LLSD((S32)i));
                    break;
                }
            }
            for (const char* name : CHIP_NAMES)
            {
                if (ChipCombo* chip = top->findChild<ChipCombo>(name))
                {
                    chip->fit();
                }
            }
        }

        // --- the icons ----------------------------------------------------------------------------------

        // The label under the focus frame, made a few times a second.
        std::string frameLabel()
        {
            if (fl::state() != fl::State::Locked)
            {
                return std::string();
            }
            const fl::Mode mode = fl::getMode();
            if (mode == fl::MODE_CURSOR)
            {
                return str("FrameCursor");
            }
            LLStringUtil::format_map_t args;
            F32 meters = 0.f;
            args["[DIST]"] = fl::focusDistance(meters) ? llformat("%.1f", meters) : std::string("-");
            if (mode == fl::MODE_CLICK)
            {
                LLPanel* hud = sS.hud.get();    // research/264: the word is the HUD panel's (en / ja)
                return hud ? hud->getString("FrameClick", args) : std::string();
            }
            args["[WHO]"]  = fl::focusName();
            args["[PART]"] = str(fl::getPart() == fl::PART_EYES ? "PartEyes" : fl::getPart() == fl::PART_CHEST ? "PartChest" : "PartBody");
            return str(mode == fl::MODE_SELF ? "FrameSelf" : "FrameLocked", args);
        }

    }

    void bindTopPanel()
    {
        LLPanel* top = sS.top.get();
        fillChips(top);
        top->getChild<LLComboBox>("chip_look")->setCommitCallback([](LLUICtrl* c, const LLSD&) {
            const size_t i = (size_t)llmax(0, c->getValue().asInteger());
            if (i < sS.lookIds.size())
            {
                pm::applyLook(sS.lookIds[i]);
            }
        });
        top->getChild<LLComboBox>("chip_res")->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedSettings.setS32(SETTING_SIZE, c->getValue().asInteger()); });
        top->getChild<LLComboBox>("chip_aspect")->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedSettings.setS32(SETTING_ASPECT, c->getValue().asInteger()); });
        top->getChild<LLComboBox>("chip_grid")->setCommitCallback([](LLUICtrl* c, const LLSD&) {
            const S32 kind = c->getValue().asInteger();
            gSavedSettings.setBOOL(SETTING_GUIDE_ON, kind > 0);
            if (kind > 0)
            {
                gSavedSettings.setS32(SETTING_GUIDE_KIND, kind);
            }
        });
        refreshChips();     // the chips' widths for their first rows
    }

    void bindHudPanel()
    {
        LLPanel* hud = sS.hud.get();
        hud->getChild<LLLineEditor>("chat_input")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
            plus::photoscreen::sendChatLine(static_cast<LLLineEditor*>(ctrl));
        });
        hud->getChild<LLUICtrl>("b_shoot")->setCommitCallback([](LLUICtrl*, const LLSD&) { plus::photomode::takePhoto(); });
        hud->getChild<LLUICtrl>("b_focus")->setCommitCallback([](LLUICtrl*, const LLSD&) {
            if (sS.wide)
            {
                // The focus settings are the right column's lens section (research/264): open it there and
                // leave the left tab as it is.
                openRightSection(SecCamera);
                return;
            }
            // Narrow (no right column): as before, the folded left panel opens over the world.
            sS.flyout = true;
            sS.tab    = TabFocus;
            fillLeft();
            relayout();
        });
        // A toggle: the click has flipped its value already.
        hud->getChild<LLUICtrl>("b_area")->setCommitCallback([](LLUICtrl* c, const LLSD&) {
            gSavedSettings.setBOOL(SETTING_FRAME, c->getValue().asBoolean());
        });
        hud->getChild<LLUICtrl>("b_full")->setCommitCallback([](LLUICtrl*, const LLSD&) { later([]() { plus::photoscreen::setSimple(true); }); });
        // The flycam on or off; the icon and the line follow in the tick (also when a key or a 3D mouse switched it).
        hud->getChild<LLUICtrl>("b_flycam")->setCommitCallback([](LLUICtrl*, const LLSD&) {
            plus::flycammouse::toggleFromUi();
            sFlycamShown = -1;
        });
        sReadout = ReadoutShown();  // the read-out is written at the first tick
        sFlycamShown = -1;
        sFocusTipWide = -1;
        if (const LLView* badge = hud->findChildView("flycam_badge", false))
        {
            const LLView* text = badge->findChildView("flycam_badge_text", false);
            sBadgeMinW = badge->getRect().getWidth();
            sBadgePad  = text ? text->getRect().mLeft : 0;
        }
    }

    // Inside the world at its top left (research/221 8.), up to the histogram's frame at the top right.
    namespace
    {
        // The chips' row inside the world, at its top left (left of the histogram's frame).
        LLRect chipsRect()
        {
            const S32 left  = sS.world.mLeft + WORLD_INSET;
            const S32 right = llmax(left, sS.world.mRight - WORLD_INSET - HIST_W - WORLD_INSET);
            const S32 high  = sS.world.mTop - WORLD_INSET;
            return LLRect(left, high, right, high - CHIP_ROW_H);
        }

        // The histogram's frame at the top right of the world, under the exit button (its place in panel_plus_photoscreen_hud.xml).
        LLRect histogramRect()
        {
            const S32 right = sS.world.mRight - WORLD_INSET;
            const S32 high  = sS.world.mTop - WORLD_INSET - EXIT_SIZE - EXIT_GAP;
            return LLRect(right - HIST_W, high, right, high - HIST_H);
        }
    }

    void layoutTop(const Margins&)
    {
        if (LLPanel* top = sS.top.get())
        {
            top->setShape(chipsRect());
        }
    }

    S32 topTaken(S32 left, S32 right)
    {
        if (!sS.shown)
        {
            return 0;
        }
        S32 taken = 0;
        for (const LLRect& part : { chipsRect(), histogramRect() })
        {
            if (left < part.mRight && right > part.mLeft)
            {
                taken = llmax(taken, sS.world.mTop - part.mBottom);
            }
        }
        return taken;
    }

    void layoutHud()
    {
        if (LLPanel* hud = sS.hud.get())
        {
            hud->setShape(sS.world);
            S32 width = llmin(CHAT_W, sS.world.getWidth() * 45 / 100);
            // the flycam's line stands at the chat input's height at the right: in a narrow world the chat ends before it
            const LLView* badge = hud->findChildView("flycam_badge", false);
            if (badge && badge->getVisible())
            {
                width = llclamp(badge->getRect().mLeft - CHAT_BADGE_GAP - WORLD_INSET, 0, width);
            }
            const S32 height = llclamp(sS.world.getHeight() - CHAT_BOTTOM - CHAT_TOP_ROOM, 0, CHAT_MAX_H);
            hud::setChatRect(sS.hudDraw.get(), LLRect(WORLD_INSET, CHAT_BOTTOM + height, WORLD_INSET + width, CHAT_BOTTOM));
            if (LLView* box = hud->findChildView("chat_box", false))     // the input inside follows its width
            {
                box->setShape(LLRect(WORLD_INSET, CHAT_INPUT_BOTTOM + CHAT_INPUT_H, WORLD_INSET + width, CHAT_INPUT_BOTTOM));
            }
            // the focus icon opens the right column's lens section, or the left panel over the world when narrow
            if ((int)sS.wide != sFocusTipWide)
            {
                sFocusTipWide = (int)sS.wide;
                if (LLView* focus = hud->findChildView("b_focus"))
                {
                    focus->setToolTip(hud->getString(sS.wide ? "FocusTipWide" : "FocusTipNarrow"));
                }
            }
        }
    }

    void tickCenter(LLPanel* hud)
    {
        refreshChips();
        refreshReadout(hud);
        refreshFlycam(hud);

        const std::string label = frameLabel();
        if (label != sS.labelShown)
        {
            sS.labelShown = label;
            hud::setFrameLabel(label);
        }
        static LLCachedControl<bool> frame_on(gSavedSettings, SETTING_FRAME, true);
        if (LLUICtrl* area = hud->findChild<LLUICtrl>("b_area"))
        {
            if (area->getValue().asBoolean() != (bool)frame_on)
            {
                area->setValue((bool)frame_on);
            }
        }
    }
}
