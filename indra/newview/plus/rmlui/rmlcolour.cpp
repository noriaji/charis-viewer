/**
 * @file rmlcolour.cpp
 * @brief ColourLink: the official colour picker for a colour square of an RmlUi window (research/380 3.1).
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

#include "rmlcolour.h"

#include "rmlhost.h"

#include "llcolorswatch.h"
#include "llfloater.h"
#include "lluictrlfactory.h"

namespace plus::rmlui
{
    class HiddenSwatch : public LLColorSwatchCtrl
    {
        friend class ::LLUICtrlFactory;

    public:
        void setFocusTarget(const LLHandle<LLRmlView>& host) { mHost = host; }

        // Visible (the picker's cone to its swatch needs that) but nothing of it is seen or touched: the document's
        // colour square is what the user sees and presses.
        void draw() override {}
        bool handleMouseDown(S32, S32, MASK) override { return false; }
        bool handleMouseUp(S32, S32, MASK) override { return false; }
        bool handleDoubleClick(S32, S32, MASK) override { return false; }
        bool handleHover(S32, S32, MASK) override { return false; }
        bool handleUnicodeCharHere(llwchar) override { return false; }

        // The official swatch focuses itself when its picker closes (select, cancel). The window's view takes
        // that focus; a disabled swatch (its picker was closed because the setting that enables it went false) and a
        // window that is going (no target) take none.
        void setFocus(bool focus) override
        {
            if (!focus)
            {
                LLColorSwatchCtrl::setFocus(false);
                return;
            }
            if (!getEnabled())
            {
                return;
            }
            if (LLRmlView* host = mHost.get())
            {
                host->setFocus(true);
            }
        }

    protected:
        explicit HiddenSwatch(const Params& params) : LLColorSwatchCtrl(params) {}

    private:
        LLHandle<LLRmlView> mHost;
    };

    HiddenSwatch* ColourLink::swatch() const
    {
        return static_cast<HiddenSwatch*>(mSwatch.get());
    }

    void ColourLink::create(LLFloater* owner, LLRmlView* host, LLControlGroup& group, const char* colour_setting,
                            const char* enabled_setting, const LLRect& rect)
    {
        if (!owner || !host || swatch())
        {
            return;
        }
        LLControlVariablePtr colour = group.getControl(colour_setting);
        if (colour.isNull())
        {
            LL_WARNS("RmlUi") << "RmlUi: the colour setting " << colour_setting << " does not exist" << LL_ENDL;
            return;
        }
        LLColorSwatchCtrl::Params params;
        params.name("rml_colour_link");
        params.rect(rect);
        params.label_height(0);
        params.mouse_opaque(false);
        params.border.mouse_opaque(false);      // its border child takes the right and middle buttons, tool tips and drops
        params.tab_stop(false);
        HiddenSwatch* hidden = LLUICtrlFactory::create<HiddenSwatch>(params);
        hidden->setFocusTarget(host->getRmlHandle());
        owner->addChild(hidden);    // the picker becomes the owner's dependent floater (showPicker)
        hidden->setControlVariable(colour);
        LLControlVariablePtr enabled = group.getControl(enabled_setting);
        if (enabled.notNull())
        {
            hidden->setEnabledControlVariable(enabled);
        }
        mSwatch = hidden->getHandle();
    }

    void ColourLink::open()
    {
        HiddenSwatch* hidden = swatch();
        if (!hidden || !hidden->getEnabled())
        {
            return;
        }
        // as the official swatch's click: focus, then the picker (which does not take the focus)
        hidden->setFocus(true);
        hidden->showPicker(false);
    }

    void ColourLink::shutdown()
    {
        if (HiddenSwatch* hidden = swatch())
        {
            hidden->setFocusTarget(LLHandle<LLRmlView>());
        }
    }
}
