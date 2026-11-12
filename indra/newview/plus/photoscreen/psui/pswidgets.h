/**
 * @file pswidgets.h
 * @brief The photo screen's widgets: XUI tags that draw the target picture's parts (research/221).
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

// Layout, input, focus, tool tips and scrolling are the official XUI's (research/221, way C):
// a panel's XML places these widgets by name and rectangle and gives their words; how they look
// comes from psstyle.h only. Each widget draws itself in draw() from members that change when
// its value changes (no allocation while drawing; setValue() with the same value does nothing).
//
// Values and commits follow the official widgets: getValue()/setValue() hold the value, a change
// by the user calls the commit callback (commit_callback in the XML or setCommitCallback()) and
// the control of control_name. Buttons, chips, cards and the shutter commit on a click (their
// value does not change). Tabs (rail and bottom) are a group with their siblings of the same
// class: choosing one clears the others and commits the one chosen.
//
// Tags (each class has its own Params, as the widget registry needs; research/149 (J)):
//   plus_ps_box          a panel's ground (kind: panel, rail, ground, glass, world_corners)
//   plus_ps_row          lays its children out from left to right (kind tabs: with the line under them)
//   plus_ps_rail_tab     a tab of the left rail: icon and up to two lines
//   plus_ps_header       a heading (size: panel, right, sub), open and closed by a click
//   plus_ps_label        a line of text (kind: label, white, dim, sub, hint, title)
//   plus_ps_divider      a thin line between sections
//   plus_ps_check        a check box with its label
//   plus_ps_switch       an on / off switch
//   plus_ps_slider       label, track and value box (the box takes typed numbers)
//   plus_ps_combo        a drop-down (the official one inside; its closed box drawn here)
//   plus_ps_chip         a chip in the world: icon, text, chevron
//   plus_ps_icon_button  an icon button in the world
//   plus_ps_histogram    the histogram's frame and curves (draws nothing until it has data)
//   plus_ps_readout      the exposure read-out and the shade under it
//   plus_ps_card         a card with a picture (kind thumb: location, sky; kind avatar)
//   plus_ps_picture      a picture with rounded corners (the time of day strip)
//   plus_ps_preset_card  a preset: picture, two lines, chosen / hovered, delete
//   plus_ps_add_card     "+ save the current settings"
//   plus_ps_segment      a choice of two or more on one bar (photo / 360, the quality's levels)
//   plus_ps_shutter      the round shutter button
//   plus_ps_button       a button
//   plus_ps_tab          a tab of the bottom strip
//   plus_ps_list_row     a row of a list: name, chosen mark, favourite star
//   plus_ps_tabs         the official tab container with its tabs drawn here (pstabs.h)
//
// Search: every widget here is an ll::ui::SearchableControl (its label and tool tip), as the official
// widgets are, so the preferences' "Search Settings" finds it, shows the tab it is on and marks it
// (drawSearchHighlight(): look::searchHit() of pscompat.h, as the compatible widgets).

#ifndef PLUS_PSWIDGETS_H
#define PLUS_PSWIDGETS_H

#include "llcombobox.h"
#include "llframetimer.h"
#include "llpanel.h"
#include "llsearchablecontrol.h"
#include "lluictrl.h"
#include "lluiimage.h"

#include "psstyle.h"   // PictureFit

#include <array>
#include <functional>
#include <string>
#include <vector>

class LLLineEditor;
class LLTexture;

namespace plus::psui
{
    // hidden is about to be hidden (a page or a fold): a widget inside it keeps no keyboard focus (a psui
    // widget does not take the focus on a click, so Space or typing would still reach the hidden one). It goes
    // to `to` (the tab or heading clicked, the tab container), or to nothing when that cannot take it.
    void releaseFocusIn(const LLView* hidden, LLUICtrl* to);

    // ---- the common part of the drawn widgets ----------------------------------------------
    class PsCtrl : public LLUICtrl, public ll::ui::SearchableControl
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
        {
            Optional<std::string> icon;     // an icon of make_ps_images.py ("camera", ...)
            Params();
        };

        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseCaptureLost() override;

        const std::string& getLabelText() const { return mLabel; }
        void setLabelText(const std::string& label);
        void setIcon(const std::string& name);
        // A key, asked before the widget's own (Space / Return): true when it was taken (the arrow keys of a list
        // made of rows). It must not delete the widget then and there (later() it).
        void setKeyCallback(std::function<bool(KEY, MASK)> cb) { mOnKey = std::move(cb); }

    protected:
        explicit PsCtrl(const Params& p);

        // A click (mouse released inside, or Space / Return with the keyboard focus: x, y < 0).
        virtual void onClick(S32 x, S32 y) {}
        // false: Return does not click (a check box or switch changes with Space only, as the official check box does).
        virtual bool clicksOnReturn() const { return true; }
        virtual void onLabelChanged() {}

        // The value changed by the user: the control of control_name, then the commit callback.
        void commitChange();
        // Sets false the siblings of the same class whose value is true (a group of tabs).
        void clearSiblingValues();
        // Takes the keyboard focus while a sibling of the same class has it (a tab of a group chosen with the
        // mouse: the keys go on from the tab chosen, and the one chosen before keeps no focus ring).
        void takeFocusFromSibling();
        // Another child of the same parent and of the same class (one of this widget's group).
        bool isSibling(const LLView* view) const;
        // The opacity to draw with: the floater's fading and 40 % when disabled.
        F32 drawAlpha() const;
        void drawFocusRing(const LLRectf& r, F32 radius) const;
        // The search's mark over r while the preferences' search matched this widget (drawn last).
        void drawSearchHighlight(const LLRectf& r) const;
        LLRectf localRectf() const;

        // ll::ui::SearchableControl: what the search looks in (the label and the tool tip, as LLCheckBoxCtrl).
        std::string _getSearchText() const override { return mLabel + getToolTip(); }

        LLPointer<LLUIImage> mIcon;
        std::string mLabel;
        LLWString   mLabelW;
        bool mHover = false;
        bool mPressed = false;

    private:
        std::function<bool(KEY, MASK)> mOnKey;
    };

    // ---- boxes ---------------------------------------------------------------------------
    class PsBox : public LLPanel
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLPanel::Params>
        {
            Optional<std::string> kind,         // panel (default), rail, ground, glass, world_corners
                                  corners;      // all (default), none, top, bottom, left, right, "tl,br" ...
            Params();
        };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsBox(const Params& p);

    private:
        enum class Kind { PANEL, RAIL, GROUND, GLASS, WORLD_CORNERS };
        Kind mKind;
        U8   mCorners;
    };

    class PsRow : public LLPanel
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLPanel::Params>
        {
            Optional<std::string> kind;         // none (default), tabs
            Optional<S32>         spacing,      // between the children
                                  pad;          // before the first one
            Params();
        };
        bool postBuild() override;
        void draw() override;
        // Puts the visible children next to each other in the order of their left edges.
        void arrange();

    protected:
        friend class LLUICtrlFactory;
        explicit PsRow(const Params& p);

    private:
        bool mTabs;
        S32  mSpacing;
        S32  mPad;
    };

    // ---- the rail and headings --------------------------------------------------------------
    class PsRailTab : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;
        void setValue(const LLSD& value) override;
        LLSD getValue() const override { return mSelected; }
        // A count in a badge at the icon's top right (0: none). The conversations tab's unread messages.
        void setBadge(S32 count);

    protected:
        friend class LLUICtrlFactory;
        explicit PsRailTab(const Params& p);
        void onClick(S32 x, S32 y) override;
        void onLabelChanged() override;

    private:
        bool mSelected = false;
        S32  mBadge = 0;
        LLWString mBadgeText;
        const LLFontGL* mFont;
        std::array<LLWString, 2> mLines;
    };

    class PsHeader : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> size,         // panel (default), right, sub
                                  divider;      // none (default), top, bottom
            Optional<bool>        collapsible;  // default: true but for sub
            Params();
        };
        void draw() override;
        void setValue(const LLSD& value) override;
        LLSD getValue() const override { return mOpen; }   // open

    protected:
        friend class LLUICtrlFactory;
        explicit PsHeader(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        enum class Size { PANEL, RIGHT, SUB };
        Size mSize;
        S32  mDivider;          // 0 none, 1 top, -1 bottom
        bool mCollapsible;
        bool mOpen = true;
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mChevronUp, mChevronDown;
    };

    class PsLabel : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> kind,         // label (default), white, dim, sub, hint, title
                                  align;        // left (default), center, right
            Params();
        };
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        void setValue(const LLSD& value) override { setLabelText(value.asString()); }
        LLSD getValue() const override { return mLabel; }
        const LLFontGL* getFont() const { return mFont; }   // the kind's font (to fit a box to the text)

    protected:
        friend class LLUICtrlFactory;
        explicit PsLabel(const Params& p);
        void onLabelChanged() override;

    private:
        void updateToolTip();
        const LLFontGL* mFont;
        LLColor4 mColor;
        LLFontGL::HAlign mAlign;
        bool mAutoToolTip;
    };

    class PsDivider : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> kind;         // left (default), right
            Optional<S32>         margin;
            Params();
        };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsDivider(const Params& p);

    private:
        LLColor4 mColor;
        S32 mMargin;
    };

    // ---- controls ---------------------------------------------------------------------------
    class PsCheck : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;
        void setValue(const LLSD& value) override { mOn = value.asBoolean(); }
        LLSD getValue() const override { return mOn; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsCheck(const Params& p);
        void onClick(S32 x, S32 y) override;
        bool clicksOnReturn() const override { return false; }

    private:
        bool mOn = false;
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mMark;
    };

    class PsSwitch : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;
        void setValue(const LLSD& value) override;
        LLSD getValue() const override { return mOn; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsSwitch(const Params& p);
        void onClick(S32 x, S32 y) override;
        bool clicksOnReturn() const override { return false; }

    private:
        F32 position() const;   // 0 off .. 1 on, moving for SWITCH_SECONDS after a click
        bool mOn = false;
        F32  mFrom = 0.f;
        bool mMoving = false;
        LLFrameTimer mTimer;
    };

    class PsSlider : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<F32>         min_value, max_value, increment;
            Optional<S32>         decimal_digits,
                                  label_width,  // default style::SLIDER_LABEL_WIDTH; 0 for no label
                                  value_width,  // default style::VALUE_WIDTH; 0 for no value box
                                  ticks;        // dots under the track (0: none)
            Optional<std::string> unit;         // after the number: " mm", "°", " m"
            Optional<bool>        show_sign;    // "+0.2"
            Optional<F32>         default_value; // the "D" button resets to this; absent: no button
            Params();
        };
        void draw() override;
        void reshape(S32 width, S32 height, bool called_from_parent = true) override;
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        bool handleToolTip(S32 x, S32 y, MASK mask) override;     // the "D" button's own tip
        void setValue(const LLSD& value) override;
        LLSD getValue() const override { return mValue; }

        // Text shown in the value box instead of the number ("18:30"); empty: the number again.
        void setDisplayText(const std::string& text);
        F32 getMinValue() const { return mMin; }
        F32 getMaxValue() const { return mMax; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsSlider(const Params& p);

    public:
        ~PsSlider() override;

    private:
        F32  snap(F32 v) const;             // to the increment, except at the default (kept as it is)
        F32  stepFrom(F32 steps, F32 step) const;   // the value `steps` keys away (from an off-step default: the next step)
        bool isDefault(F32 v) const;        // v is the default, give or take the float rounding of a getter
        bool setValueInternal(F32 v);       // true when it changed
        void updateText();
        F32  trackLeft() const;
        F32  trackRight() const;
        F32  knobX() const;
        LLRectf valueRect() const;
        // The "D" button's square, right of the value box (or at the slider's right with no value box);
        // empty (width 0) while there is no default.
        LLRectf defaultRect() const;
        void drawDefaultButton(F32 alpha) const;
        void beginEdit();
        void endEdit();

        F32 mMin, mMax, mIncrement, mValue;
        S32 mDecimals, mLabelWidth, mValueWidth, mTicks;
        std::string mUnit;
        bool mShowSign;
        bool mHasDefault;
        F32  mDefaultValue;                 // clamped to min..max; not snapped (0.325 on a 0.05 grid stays 0.325)
        F32  mDefaultEps;                   // isDefault()'s margin, well under half an increment
        bool mDefaultHover = false;
        bool mDragging = false;
        bool mEditing = false;
        std::string mDisplay;
        LLWString mValueText;
        const LLFontGL* mFont;
        LLLineEditor* mEditor = nullptr;
    };

    class PsCombo : public LLComboBox
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLComboBox::Params> { Params(); };
        void draw() override;
        void onMouseEnter(S32 x, S32 y, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        // The shown text is read again when the chosen row or the number of rows changes;
        // after renaming the chosen row in place, call this.
        void refreshShown();

    protected:
        friend class LLUICtrlFactory;
        explicit PsCombo(const Params& p);

    private:
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mChevron;
        LLWString mShown;
        S32 mShownIndex = -2;
        S32 mShownCount = -1;
        bool mHover = false;
    };

    // ---- the world's overlay ------------------------------------------------------------------
    class PsChip : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<bool> dropdown,            // a chevron after the text
                           auto_width;          // the width follows the text (default true)
            Params();
        };
        void draw() override;
        void setValue(const LLSD& value) override { setLabelText(value.asString()); }
        LLSD getValue() const override { return mLabel; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsChip(const Params& p);
        void onClick(S32 x, S32 y) override { commitChange(); }
        void onLabelChanged() override;

    private:
        void fitWidth();
        bool mDropdown;
        bool mAutoWidth;
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mChevron;
    };

    class PsIconButton : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<S32>  icon_size;
            Optional<bool> toggle,              // a click flips the value (on: the icon is blue)
                           star;                // a favourite's star: star / star_filled, the list rows' colours
                                                // (the photo tools' "show on Home"; icon is not read)
            Params();
        };
        void draw() override;
        void setValue(const LLSD& value) override { mOn = value.asBoolean(); }
        LLSD getValue() const override { return mOn; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsIconButton(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        S32  mIconSize;
        bool mToggle;
        bool mStar;
        bool mOn = false;
        LLPointer<LLUIImage> mStarFilled;   // the star's icon while on (mIcon: off)
    };

    class PsHistogram : public PsCtrl
    {
    public:
        static constexpr int BINS = 64;
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;
        // Counts per level, dark to bright (count values each; resampled to BINS). Null: no data
        // (the frame is not drawn either).
        void setData(const F32* red, const F32* green, const F32* blue, S32 count);
        bool hasData() const { return mHasData; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsHistogram(const Params& p);

    private:
        bool mHasData = false;
        std::array<F32, BINS> mRed {}, mGreen {}, mBlue {}, mLuma {};   // 0..1
    };

    class PsReadout : public PsCtrl
    {
    public:
        static constexpr int ITEMS = 4;
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };   // label: items joined by '|'
        void draw() override;
        void setItem(int index, const std::string& text);

    protected:
        friend class LLUICtrlFactory;
        explicit PsReadout(const Params& p);

    private:
        const LLFontGL* mFont;
        std::array<std::string, ITEMS> mItems;
        std::array<LLWString, ITEMS> mItemsW;
        std::array<S32, ITEMS> mWidths {};
    };

    // ---- cards and pictures -----------------------------------------------------------------
    class PsCard : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> kind,         // thumb (default), avatar
                                  subtitle;
            Optional<bool>        more;         // the "..." at the right (default: thumb only)
            Optional<LLUIImage*>  image;
            Params();
        };
        void draw() override;
        void setSubtitle(const std::string& text);
        void setTexture(const LLPointer<LLTexture>& texture) { mTexture = texture; }
        // Drawn as the one chosen (an accent border), as a card that switches what is shown under it.
        void setChosen(bool chosen) { mChosen = chosen; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsCard(const Params& p);
        void onClick(S32 x, S32 y) override { commitChange(); }

    private:
        bool mAvatar;
        bool mMore;
        bool mChosen = false;
        std::string mSubtitle;
        LLWString mSubtitleW;
        LLPointer<LLTexture> mTexture;
        const LLFontGL* mTitleFont;
        const LLFontGL* mSubFont;
        LLPointer<LLUIImage> mMoreIcon;
    };

    class PsPicture : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<LLUIImage*>  image;
            Optional<F32>         radius;
            Optional<std::string> placeholder;  // icon (default: the icon on a dark ground), dusk_night
            Params();
        };
        void draw() override;
        void setTexture(const LLPointer<LLTexture>& texture) { mTexture = texture; }
        // PictureFit::Stretch: the whole picture in the rectangle instead of cut to its shape
        // (for a rectangle already made with the picture's aspect).
        void setFit(PictureFit fit) { mFit = fit; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsPicture(const Params& p);

    private:
        LLPointer<LLTexture> mTexture;
        F32 mRadius;
        bool mDuskNight;
        PictureFit mFit = PictureFit::Crop;
    };

    class PsPresetCard : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> subtitle;
            Optional<LLUIImage*>  image;
            Optional<bool>        deletable;    // an "x" while hovered
            Params();
        };
        void draw() override;
        void setValue(const LLSD& value) override { mChosen = value.asBoolean(); }  // the current settings
        LLSD getValue() const override { return mChosen; }
        void setSubtitle(const std::string& text);
        void setTexture(const LLPointer<LLTexture>& texture) { mTexture = texture; }
        void setDeleteCallback(std::function<void(PsPresetCard*)> cb) { mOnDelete = std::move(cb); }

    protected:
        friend class LLUICtrlFactory;
        explicit PsPresetCard(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        LLRectf pictureRect() const;
        LLRectf closeRect() const;
        bool mChosen = false;
        bool mDeletable;
        std::string mSubtitle;
        LLWString mSubtitleW;
        LLPointer<LLTexture> mTexture;
        const LLFontGL* mTitleFont;
        const LLFontGL* mSubFont;
        LLPointer<LLUIImage> mClose;
        std::function<void(PsPresetCard*)> mOnDelete;
    };

    class PsAddCard : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsAddCard(const Params& p);
        void onClick(S32 x, S32 y) override { commitChange(); }

    private:
        const LLFontGL* mFont;
    };

    // ---- the shutter strip and buttons ------------------------------------------------------
    // A choice of two or more on one bar (photo / 360, the quality's levels, the gaze). The value is the
    // index of the choice, 0 at the left; -1 is none chosen (setValue only). Two choices from left_label and
    // right_label, more from labels ("Low|Mid|High") or setChoices(). With three or more, Left / Right,
    // Home / End choose too (as a radio group); Space / Return steps to the next one (two: flips).
    class PsSegment : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<std::string> left_label, right_label,
                                  labels;       // the choices joined by '|' (instead of left / right)
            Params();
        };
        void draw() override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        void setValue(const LLSD& value) override;
        LLSD getValue() const override { return mIndex; }

        // The choices' words (at least two are kept; a missing one is empty). The value stays if it still fits.
        void setChoices(const std::vector<std::string>& labels);
        // A tip for each choice, shown while the mouse is over it (empty: the widget's own tip).
        void setChoiceToolTips(const std::vector<std::string>& tips);
        S32  getChoiceCount() const { return (S32)mLabels.size(); }

    protected:
        friend class LLUICtrlFactory;
        explicit PsSegment(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        S32  indexAt(S32 x) const;
        F32  edge(S32 index) const;         // the left edge of a choice (count: the right end)
        void choose(S32 index);             // by the user: commits when it changed
        void showHoverTip();

        S32 mIndex = 0;
        S32 mHoverIndex = -1;
        std::vector<LLWString> mLabels;
        std::vector<std::string> mTips;
        std::string mOwnTip;                // the widget's tip, while no choice's tip is shown
        const LLFontGL* mFont;
    };

    class PsShutter : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;
        // Shows the button as busy (a picture being taken) for SHUTTER_BUSY_SECONDS.
        void flash();

    protected:
        friend class LLUICtrlFactory;
        explicit PsShutter(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        LLFrameTimer mBusy;
        bool mBusyShown = false;
    };

    class PsButton : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params> { Params(); };
        void draw() override;

    protected:
        friend class LLUICtrlFactory;
        explicit PsButton(const Params& p);
        void onClick(S32 x, S32 y) override { commitChange(); }

    private:
        const LLFontGL* mFont;
    };

    class PsTab : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<bool> auto_width;          // the width follows the text (default true)
            Params();
        };
        void draw() override;
        void setValue(const LLSD& value) override { mSelected = value.asBoolean(); }
        LLSD getValue() const override { return mSelected; }

    protected:
        friend class LLUICtrlFactory;
        explicit PsTab(const Params& p);
        void onClick(S32 x, S32 y) override;
        void onLabelChanged() override;

    private:
        void fitWidth();
        bool mSelected = false;
        bool mAutoWidth;
        const LLFontGL* mFont;
    };

    // ---- lists ---------------------------------------------------------------------------------
    // A row of a list: its name, the chosen row's mark, and a star at the right (a favourite). A click
    // on the row commits (its value, chosen, is the caller's to set); a click on the star calls the
    // star callback. A missing row (a favourite whose item is gone) is dim and only its star works.
    class PsListRow : public PsCtrl
    {
    public:
        struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
        {
            Optional<bool> starrable;           // a star at the right (default true)
            Params();
        };
        void draw() override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        // Return / Space: the row (as a click); Shift+Return / Shift+Space: the star.
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        void setValue(const LLSD& value) override { mChosen = value.asBoolean(); }  // the one in use
        LLSD getValue() const override { return mChosen; }
        void setStarred(bool starred) { mStarred = starred; }
        void setMissing(bool missing) { mMissing = missing; }
        void setStarCallback(std::function<void(PsListRow*)> cb) { mOnStar = std::move(cb); }
        // A count in a badge at the right, where a row that is not starrable has room (0: none); quiet: grey.
        void setBadge(S32 count, bool quiet);

    protected:
        friend class LLUICtrlFactory;
        explicit PsListRow(const Params& p);
        void onClick(S32 x, S32 y) override;

    private:
        LLRectf starRect() const;
        bool mStarrable;
        bool mChosen = false;
        bool mStarred = false;
        bool mMissing = false;
        bool mStarHover = false;
        S32  mBadge = 0;
        bool mBadgeQuiet = false;
        LLWString mBadgeText;
        const LLFontGL* mFont;
        LLPointer<LLUIImage> mStar, mStarFilled;
        std::function<void(PsListRow*)> mOnStar;
    };
}

#endif // PLUS_PSWIDGETS_H
