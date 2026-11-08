/**
 * @file pscompat.h
 * @brief The official widgets in the photo screen's look: the compatible psui widgets (research/266 2.1, 239 (3)).
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

// The official windows' C++ looks its widgets up by type (getChild<LLCheckBoxCtrl>("..."), research/239 (3));
// a psui widget of another class is not found and a dummy takes its place. The widgets here derive from the
// official classes, as PsCombo does: input, values, the control of control_name, commit callbacks, dirty
// (LLPanelPreferenceGraphics::hasDirtyChilds) and the preferences' search (ll::ui::SearchableControl) are
// the official widget's; draw() alone is the photo screen's look, from the looks below (the same functions
// draw the photo screen's own widgets, so a look is written once). The official children (the check box's
// button and label, the slider's bar and value editor ...) stay and keep the input; they are placed where
// the look draws its parts, and are not drawn (the value editor only while it is typed in).
//
// Tags (the names of tools/ui_ledger.py COMPAT, which tools/xui2psui.py writes):
//   plus_ps_button_compat  LLButton        a button (toggle: blue while on); icon="<psui icon>" before its words
//   plus_ps_check_compat   LLCheckBoxCtrl  a check box with its label
//   plus_ps_slider_compat  LLSliderCtrl    label, track and value box (the box takes typed numbers); ticks
//   plus_ps_spinner        LLSpinCtrl      label and a value box with up / down arrows
//   plus_ps_text           LLTextBox       text (kind: label, white, dim, sub, hint, title), links and lines as official
//   plus_ps_radio          LLRadioGroup    a choice of its radio_item children (kind: list = round marks where the
//                                          items are placed, segment = one bar as plus_ps_segment)
// plus_ps_combo (LLComboBox) is in pswidgets.h; plus_ps_tabs (LLTabContainer) in pstabs.h (branch prefs-frame).
// The wheel over a slider or a spinner does not change it: the panel under it scrolls (a spinner's value
// changes with the wheel only while it is typed in).

#ifndef PLUS_PSCOMPAT_H
#define PLUS_PSCOMPAT_H

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llradiogroup.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluiimage.h"

#include <string>
#include <vector>

class LLLineEditor;
class LLSlider;

namespace plus::psui
{
    // ---- the looks: drawn by the photo screen's widgets and the compatible ones ----------------
    // Rectangles are in the widget's own coordinates; alpha is the widget's (drawAlphaOf: the fading and the
    // disabled state); focus is the widget that has the keyboard focus while the ring is to be drawn.
    namespace look
    {
        // A check box at the left of r, centred, and its label after it (4.).
        void check(const LLRectf& r, bool on, bool hover, bool pressed, F32 alpha, const LLFontGL* font,
                   const LLWString& label, const LLUIImage* mark, const LLUICtrl& focus);
        // A round mark at the left of r and its label after it: a choice of a list (the check box's colours).
        void radio(const LLRectf& r, bool on, bool hover, bool pressed, F32 alpha, const LLFontGL* font,
                   const LLWString& label, const LLUICtrl& focus);
        // A slider's track from x0 to x1 at mid, filled up to the knob at kx, ticks dots under it and the knob (5.).
        void sliderTrack(F32 x0, F32 x1, F32 kx, F32 mid, S32 ticks, bool dragging, bool hover, F32 alpha,
                         const LLUICtrl& focus);
        // The value box of a slider or a spinner, and its text at the right of it.
        void valueBox(const LLRectf& r, F32 alpha);
        void valueText(const LLRectf& r, const LLFontGL* font, const LLWString& text, F32 alpha);
        // A button's box (16.), on: a toggle that is on (blue), an icon (or none) and the words in the middle.
        void button(const LLRectf& r, bool pressed, bool hover, bool on, F32 alpha, const LLFontGL* font,
                    const LLWString& label, const LLUIImage* icon, S32 icon_size, const LLUICtrl& focus);
        // A choice of count on one bar (14.): the left edge of a choice (count: the right end), the bar with the
        // chosen one (-1: none) and a choice's words.
        F32  segmentEdge(const LLRectf& r, S32 index, S32 count);
        void segmentBar(const LLRectf& r, S32 count, S32 chosen, F32 alpha);
        void segmentLabel(const LLRectf& r, S32 index, S32 count, bool chosen, bool hovered, const LLFontGL* font,
                          const LLWString& label, F32 alpha);
        // Under a part the preferences' search found (instead of the official SearchableControlHighlight*).
        void searchHit(const LLRectf& r, F32 context_alpha);
    }

    // ---- the compatible widgets -------------------------------------------------------------------
    class PsButtonCompat : public LLButton
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLButton::Params>
        {
            Optional<std::string> icon;         // a psui icon (make_ps_images.py); the official image_overlay is drawn when there is none
            Optional<S32>         icon_size;
            Params();
        };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsButtonCompat(const Params& p);

    private:
        bool pressedNow();
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mIcon;
        S32 mIconSize;
    };

    class PsCheckCompat : public LLCheckBoxCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLCheckBoxCtrl::Params> { Params(); };
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsCheckCompat(const Params& p);

    private:
        // The button over the whole widget (all of it takes the click), the label after the drawn box.
        void placeChildren();
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mMark;
        bool mHover = false;
    };

    class PsSliderCompat : public LLSliderCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLSliderCtrl::Params>
        {
            Optional<S32> ticks;                // dots under the track (0: none)
            Params();
        };
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        void setRect(const LLRect& rect) override;
        // Home / End (the bar keeps the arrows).
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsSliderCompat(const Params& p);

    private:
        // Label | bar | gap | value box, as PsSlider draws them; the bar's knob travels as the official bar maps the mouse.
        void placeChildren();
        LLRectf valueRect() const;
        LLSlider*     mBar = nullptr;
        LLTextBox*    mLabelText = nullptr;
        LLLineEditor* mEditor = nullptr;        // can_edit_text
        LLTextBox*    mValueText = nullptr;     // show_text without can_edit_text
        LLPointer<LLUIImage> mThumb;            // the bar's thumb image: its width is the bar's travel margin
        const LLFontGL* mFont;
        S32  mLabelWidth;
        S32  mValueWidth;
        S32  mTicks;
        bool mHover = false;
    };

    class PsSpinner : public LLSpinCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLSpinCtrl::Params> { Params(); };
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsSpinner(const Params& p);

    private:
        // Label | value box with the editor and the up / down arrows at its right.
        void placeChildren();
        LLRectf valueRect() const;
        LLTextBox*    mLabelText = nullptr;
        LLLineEditor* mEditor = nullptr;
        LLButton*     mUp = nullptr;
        LLButton*     mDown = nullptr;
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mChevronUp, mChevronDown;
        S32  mLabelWidth;
        bool mHover = false;
    };

    class PsText : public LLTextBox
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLTextBox::Params>
        {
            Optional<std::string> kind;         // label (default), white, dim, sub, hint, title: as plus_ps_label
            Params();
        };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsText(const Params& p);
    };

    class PsRadio : public LLRadioGroup
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLRadioGroup::Params>
        {
            Optional<std::string> kind;         // list (default): where the items are placed; segment: one bar
            Params();
        };
        bool postBuild() override;
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        // The clicks are taken here for the whole of an item (its slot), and given to the group as the item's click.
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        // Home / End too (the group keeps the arrows).
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        void onMouseCaptureLost() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsRadio(const Params& p);

    private:
        void placeItems();                      // segment: each item on its part of the bar
        S32  itemAt(S32 x, S32 y) const;        // -1: none
        bool itemHighlighted(LLCheckBoxCtrl* item) const;

        bool mSegment;
        std::vector<LLCheckBoxCtrl*> mItems;    // the official radio items (children: live as long as the group), in the
                                                // order of the XML (the group's indices)
        S32  mHoverItem = -1;
        S32  mPressedItem = -1;
        bool mHover = false;
        const LLFontGL* mFont;
        const LLFontGL* mSegmentFont;
    };

    // One of the compatible widgets above (the parts sample greys them with the psui ones).
    bool isCompatWidget(const LLView* view);
}

#endif // PLUS_PSCOMPAT_H
