/**
 * @file photodetail.cpp
 * @brief The detailed mode of the photo screen: the panels around the world (research/218 in the
 *        sl-viewer repository).
 *
 * The files (research/221 "ps-split"; each area in its own file, so that they can be changed apart):
 *  - photodetail.cpp: this. The making of the panels, their layout around the world, the tick, place().
 *  - photodetail_state.h: the state the files share (sS) and the small helpers (defined here).
 *  - photodetail_layout.h: the sizes of the frame and of the parts, in one table.
 *  - photodetail_rows.{h,cpp}: the rows of the side panels (the photo tools' items).
 *  - photodetail_left.{h,cpp}: the rail and the tab's panel. photodetail_right.{h,cpp}: the sections.
 *  - photodetail_center.{h,cpp}: the chips, the read-out, the chat input, the icons (and photodetail_hud.cpp:
 *    the drawing view of the focus frame and the chat lines). photodetail_bottom.{h,cpp}: the preset cards.
 *
 * What is here:
 *  - Left: a rail of six tabs (focus, camera, light & color, sky & water, avatar, save) and, beside it,
 *    the tab's rows. The rows are the photo tools' own items (plus/photomode/phototools.h: Item, itemValue(),
 *    setItemValue()), so nothing is implemented twice and the two windows always show the same values;
 *    the focus tab is plus/focuslock's controls, the sky tab the favourite and recent skies and waters of
 *    plus/photomode/photosky.h.
 *  - Right: the camera settings in folding sections (camera, exposure and tone, color, effects, output).
 *    The focal length is the viewer's Field of View shown as a 35 mm lens; the f-number is the photo
 *    tools' "Background Blur" (CameraFNumber). There is no histogram: the viewer has none to reuse.
 *  - Above the world: chips (look, size, shape, composition lines) and the exit. Over the world: the read-out
 *    (F, shutter speed, ISO, focal length: the shutter speed and the ISO are for the eye, see readout()),
 *    the chat (lines and input), the icon row (shoot, focus, focus frame, HUD, full window), and the focus
 *    frame (photodetail_hud.cpp). The composition lines are plus/photomode/photoguide.cpp, which draws in the
 *    world's rectangle while the photo screen is on.
 *  - Under the world: the strip of presets (camera positions, poses, scenes, my presets) as cards. The small
 *    picture of a card is made once when the preset is saved (an idle callback, 192 px wide, kept in the
 *    user's settings folder) and read back once per session.
 *
 * Speed: place() runs when the layout changes, never per frame. A tick of a quarter of a second reads the
 * values of the rows shown (each widget is written only when its value changed), the read-out and the
 * state of the focus lock; it does nothing at all while the panels are hidden.
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

#include "photodetail.h"

#include "photodetail_bottom.h"
#include "photodetail_center.h"
#include "photodetail_hud.h"
#include "photodetail_left.h"
#include "photodetail_right.h"
#include "photodetail_rows.h"
#include "photodetail_state.h"
#include "photoscreen.h"
#include "plus/photoscreen/psui/psstack.h"

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcombobox.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluictrlfactory.h"

namespace plus::photoscreen::detail
{
    State sS;

    std::string str(const char* key)
    {
        return LLTrans::getString(std::string("PhotoScreenDetail_") + key);
    }

    std::string str(const std::string& key)
    {
        return LLTrans::getString("PhotoScreenDetail_" + key);
    }

    std::string str(const char* key, const LLStringUtil::format_map_t& args)
    {
        return LLTrans::getString(std::string("PhotoScreenDetail_") + key, args);
    }

    LLPanel* loadPanel(const char* file, LLView* parent)
    {
        return LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(file, parent, LLPanel::child_registry_t::instance());
    }

    void later(std::function<void()> work)
    {
        doOnIdleOneTime(std::move(work));
    }

    void setTextIfChanged(LLTextBox* box, const std::string& text)
    {
        if (box && box->getText() != text)
        {
            box->setText(text);
        }
    }

    void setComboIfChanged(LLComboBox* combo, const LLSD& value)
    {
        psui::setComboIfChanged(combo, value);     // one helper for the photo screen and the psui column
    }

    void setToggleIfChanged(LLButton* button, bool on)
    {
        if (button && button->getToggleState() != on)
        {
            button->setToggleState(on);
        }
    }

    namespace
    {
        // Every quarter of a second while the panels are shown.
        bool tick()
        {
            LLPanel* hud = sS.hud.get();
            if (!sS.shown || !hud || !plus::photoscreen::isActive())
            {
                sS.tickRegistered = false;
                return true;
            }
            if (!hud->getVisible() || sS.timer.getElapsedTimeF32() < TICK_SECONDS)
            {
                return false;
            }
            sS.timer.reset();

            refreshRows(sS.leftRows);
            refreshRows(sS.rightRows);
            tickLeft();
            tickCenter(hud);
            tickBottom();
            tickRight();
            return false;
        }

        // --- making the panels ---------------------------------------------------------------

        void bindPanels()
        {
            bindLeftPanel();
            bindTopPanel();
            bindBottomPanel();
            bindHudPanel();
        }

        // A panel's XML did not load: not tried again in this session (each place() would make and drop them).
        bool sLoadFailed = false;

        bool ensure(LLView* overlay)
        {
            if (sS.hud.get())
            {
                return true;
            }
            if (sLoadFailed)
            {
                return false;
            }
            LLPanel* left   = loadPanel("panel_plus_photoscreen_left.xml", overlay);
            LLPanel* right  = loadPanel("panel_plus_photoscreen_right.xml", overlay);
            LLPanel* top    = loadPanel("panel_plus_photoscreen_top.xml", overlay);
            LLPanel* bottom = loadPanel("panel_plus_photoscreen_bottom.xml", overlay);
            LLPanel* hud    = loadPanel("panel_plus_photoscreen_hud.xml", overlay);
            if (!left || !right || !top || !bottom || !hud)
            {
                LL_WARNS("PlusPhotoScreen") << "A panel of the photo screen did not load; its frame is not shown" << LL_ENDL;
                sLoadFailed = true;
                for (LLPanel* panel : { left, right, top, bottom, hud })
                {
                    if (panel)
                    {
                        panel->die();   // the ones made are not left on the overlay
                    }
                }
                return false;
            }
            sS.overlay = overlay->getHandle();
            sS.left    = left->getHandle();
            sS.right   = right->getHandle();
            sS.top     = top->getHandle();
            sS.bottom  = bottom->getHandle();
            sS.hud     = hud->getHandle();
            if (LLView* draw = hud::create(hud))
            {
                sS.hudDraw = draw->getHandle();
            }
            bindPanels();
            fillLeft();
            fillRight();
            return true;        // the cards: place(), as the panels are shown
        }

        // The panels around the world from the last place().
        void layoutPanels()
        {
            Margins m;
            if (!plus::photoscreen::margins(sS.w, sS.h, m))
            {
                return;
            }
            sS.wide = m.wide;
            const bool open = m.wide || sS.flyout;
            layoutLeft(m, open);
            layoutRight(m);
            layoutTop(m);
            layoutBottom(m);
            layoutHud();
            // The cards keep their places by the strip's reshape; they are made again only when what they show
            // changed (as the tick does), not on every layout (a window being resized).
            tickBottom();
        }
    }

    void relayout()
    {
        if (sS.shown)
        {
            layoutPanels();
        }
    }

    void place(LLView* overlay, const LLRect& world, S32 ui_w, S32 ui_h, bool simple)
    {
        if (!overlay)
        {
            return;
        }
        Margins margins;
        const bool framed = !simple && plus::photoscreen::margins(ui_w, ui_h, margins);
        if (!framed && !sS.hud.get())
        {
            return;     // nothing was made, and nothing is to be shown
        }
        if (!ensure(overlay))
        {
            return;
        }
        const bool was_shown = sS.shown;
        sS.shown = framed;
        sS.world = world;
        sS.w     = ui_w;
        sS.h     = ui_h;
        for (const LLHandle<LLPanel>* handle : { &sS.left, &sS.right, &sS.top, &sS.bottom, &sS.hud })
        {
            if (LLPanel* panel = handle->get())
            {
                panel->setVisible(framed);
            }
        }
        if (!framed)
        {
            return;
        }
        if (!was_shown || !sS.tickRegistered)
        {
            rebuildCards();     // made, or shown again (the tick had stopped): the poses' folder and the pictures read anew
        }
        layoutPanels();
        if (!sS.tickRegistered)
        {
            sS.tickRegistered = true;
            sS.timer.reset();
            doOnIdleRepeating(&tick);
        }
    }
}
