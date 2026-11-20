/**
 * @file rmlcolour.h
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

// The colour picker (LLFloaterColorPicker) opens only for an LLColorSwatchCtrl. A ColourLink makes one that is
// put where the document's colour square is, in the window (a visible child that draws nothing, takes no mouse
// and no Tab: the picker's cone points at the swatch only while it is in the visible chain), bound to the colour
// setting and to the "enabled" setting (what color_swatch with control_name and enabled_control does in an XUI
// window), and opens its picker when the document's colour square is pressed. The rest is the official swatch's:
// OK / Cancel / the picker's close, the enabling setting going false cancelling the selection and closing the
// picker, the OS colour window when UseDefaultColorPicker is set. The colour is written only when it is selected
// (can_apply_immediately is false), so a window closing with its picker open (closeFloater closes the dependents,
// with no cancelSelection) leaves the colour as it was: the same as the XUI window.
//
// The one addition: after the picker closes (select or cancel) the swatch focuses itself ("return the focus");
// the swatch gives that focus to the window's RmlUi view instead, so no key goes to a part nobody sees.
// The window calls shutdown() first in its destructor: the view is going too, and takes no focus then.

#ifndef PLUS_RMLCOLOUR_H
#define PLUS_RMLCOLOUR_H

#include "llcontrol.h"
#include "llhandle.h"
#include "llrect.h"

class LLFloater;
class LLRmlView;

namespace plus::rmlui
{
    class HiddenSwatch;

    class ColourLink
    {
    public:
        ColourLink() = default;
        ~ColourLink() { shutdown(); }
        ColourLink(const ColourLink&) = delete;
        ColourLink& operator=(const ColourLink&) = delete;

        // owner: the window that holds the picker as a dependent floater; rect: where the colour square is, in the
        // owner's coordinates (y up). A missing setting leaves the link unmade (open() then does nothing).
        void create(LLFloater* owner, LLRmlView* host, LLControlGroup& group, const char* colour_setting,
                    const char* enabled_setting, const LLRect& rect);
        // The colour square was pressed.
        void open();
        // The window is going: no focus goes to it any more. The swatch is deleted with the window's children.
        void shutdown();

    private:
        HiddenSwatch* swatch() const;
        LLHandle<LLView> mSwatch;           // weak: the window's children own it
    };
}

#endif // PLUS_RMLCOLOUR_H
