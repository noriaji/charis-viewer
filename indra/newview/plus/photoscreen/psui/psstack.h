/**
 * @file psstack.h
 * @brief A column of psui widgets stacked from the top in a scrolled panel, and the rows that follow
 *        values (the photo screen's left panel, research/221 ps-left; the photo tools, research/265), and
 *        the sub-tabs that show one page each (TabPages).
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

// How it is used (photodetail_left.cpp is the example):
//   Stack stack(content, width, rows);      // content: the panel inside a scroll container (emptied)
//   stack.header(...); stack.item(item); stack.slider(...); ... stack.endSection();
//   stack.finish();                          // places the widgets, sizes the content
//   then every tick: Stack::refresh(rows);   // each widget written only when its value changed
// The column is made again only when what it shows changes (never per frame). Two ways to lay a row out:
// the label on a line over its control (the photo screen's left panel, the default) or beside it on one
// line (setLabelsBeside(true): the right panel's sizes, research/265 2.). A caller may add marks at the
// left of each value row (setDecorator(): the photo tools' star, "changed" and "!"); the photo screen
// adds none. Folds (foldHeader() ... endFold()) open and close without making their rows again.

#ifndef PLUS_PSSTACK_H
#define PLUS_PSSTACK_H

#include "plus/photomode/phototools.h"

#include "llhandle.h"
#include "llpanel.h"
#include "llsd.h"
#include "stdtypes.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class LLComboBox;
class LLControlVariable;
class LLFontGL;
class LLLineEditor;
class LLTextBox;
class LLUICtrl;
class LLView;

namespace plus::psui
{
    class PsButton;
    class PsCard;
    class PsCheck;
    class PsCombo;
    class PsHeader;
    class PsIconButton;
    class PsLabel;
    class PsPicture;
    class PsPresetCard;
    class PsSegment;
    class PsSlider;

    // The column's sizes (UI pixels; research/221, the design parts 2.-5., 15., 16. at 1080). The widgets draw
    // themselves from psstyle.h; these only stack them.
    namespace stack
    {
        inline constexpr S32 PAD          = 14;     // left and right of the rows (the headings have their own)
        inline constexpr S32 PAD_RIGHT_MIN = 4;     // the right pad left beside the scroll bar
        inline constexpr S32 HEADER_H     = 52;     // 2. a heading
        inline constexpr S32 SUB_H        = 30;     // 2. a sub-heading (the right panel's)
        inline constexpr S32 LABEL_H      = 22;     // 3. a small label: 15 px text
        inline constexpr S32 LABEL_TOP    = 6;      //    and the room over it (10 in the picture, minus the row's own)
        inline constexpr S32 CHECK_H      = 30;     // 4. a check box's row
        inline constexpr S32 SLIDER_H     = 30;     // 5. a slider without its label (the label is a line over it)
        inline constexpr S32 COMBO_H      = 33;     // 7. a drop-down
        inline constexpr S32 COMBO_PAD    = 2;      //    over and under it
        inline constexpr S32 BUTTON_H     = 40;     // 16. a button
        inline constexpr S32 CARD_H       = 59;     // 15. a card with a picture
        inline constexpr S32 AVATAR_H     = 69;     // 15. the avatar's card
        inline constexpr S32 PICTURE_H    = 52;     // 15. the time of day strip
        inline constexpr S32 NOTE_H       = 44;     // a note of up to three short lines
        inline constexpr S32 PRESET_H     = 132;    // 18. a preset card: picture 90, two lines under it
        inline constexpr S32 PRESET_GAP   = 12;
        inline constexpr S32 ROW_GAP      = 6;      // between the blocks of a section
        inline constexpr S32 SECTION_END  = 10;     // under a section's last row

        // The rows with the label beside the control (setLabelsBeside(); the right panel's Builder sizes).
        inline constexpr S32 LINE_SLIDER_H = 31;    // label, track and value box on one line
        inline constexpr S32 STEP_W       = 35;     // the look row's arrow buttons (the right panel's icon button)
        inline constexpr S32 STEP_GAP     = 4;      //    and the room between them and the drop-down
        inline constexpr S32 TEXT_INSET   = 4;      // a typed text's box inside its glass box

        // The marks a decorator adds (setDecorator(); the photo tools' star, "changed" and "!").
        inline constexpr S32 DECOR_STAR_W = 24;
        inline constexpr S32 DECOR_MARK_W = 12;
    }

    // The value of a drop-down set only when it differs (a drop-down set to the same value redraws its text).
    void setComboIfChanged(LLComboBox* combo, const LLSD& value);

    // A note: a short text wrapped in rect, letting the mouse through (Stack::note(), and the "nothing here" line
    // of a list or a strip made outside a Stack). Made, not added: the caller adds it.
    LLTextBox* makeNote(const std::string& name, const LLRect& rect, const std::string& text, const LLFontGL* font,
                        const LLColor4& color, U32 follows = FOLLOWS_LEFT | FOLLOWS_TOP);

    namespace stack
    {
        // The marks a decorator gives a row (research/265 2.): the star that shows the row on Home (a toggle;
        // its commit callback is the caller's), DECOR_STAR_W square, and a small mark of hint text ("*"
        // changed, "!" rebuilds) with its tip, DECOR_MARK_W wide. Hidden marks keep their room.
        PsIconButton* makeStar(const std::string& tip, bool starred);
        PsLabel*      makeMark(const std::string& text, const std::string& tip);
    }

    class Stack
    {
    public:
        // A widget that shows a value read again every tick (written only when the value changed).
        struct Row
        {
            enum class Type { Check, Slider, Combo, Segment, Text };
            LLUICtrl* ctl  = nullptr;
            LLView*   label = nullptr;              // a label beside the control, greyed out with it (or null)
            Type      type = Type::Slider;
            float     scale = 1.f;                  // shown = value * scale
            std::vector<plus::photomode::Option> options;   // a combo's choices (the nearest is shown)
            std::function<float()> get;
            std::function<std::string()> text;      // a typed text's (Type::Text; get is null)
            const plus::photomode::Item* item = nullptr;    // the photo tools' item the row was made from (or null)
            LLControlVariable* needs = nullptr;     // usable only while this setting is needs_value (-1: not 0)
            int  needs_value = -1;
            bool blur = false;                      // usable only with the depth of field
            plus::photomode::FeatureGate feature;   // and with the feature it needs (catalog "needs_feature")
            bool shown_set = false;
            float shown = 0.f;
            std::string shown_text;
            int  enabled = -1;
        };

        // What a heading of the column says: its id (the open / closed state is kept by id), words and icon.
        struct Heading
        {
            std::string id, label;
            const char* icon = nullptr;
            bool        open_by_default = true;
        };

        // Makes the marks at the left of a value row (the photo tools' star, "changed" and "!"): views made
        // for the row, left to right, each as wide as its rectangle, put in the row's decoration strip. item:
        // the photo tools' item the row is made from (null for a row that is not one); row: the index of the
        // row's entry in the rows (-1 for a row without a value, the look row; an index, as the rows move in
        // memory while more are added). Called once when the row is made.
        using Decorator = std::function<void(const plus::photomode::Item* item, S32 row, std::vector<LLView*>& out)>;

        // What item() does besides making the row of a kind (all optional).
        struct ItemHooks
        {
            // Writes a value of an item's row (null: plus::photomode::setItemValue).
            std::function<void(const plus::photomode::Item&, float)> write;
            // Writes a typed text (null: plus::photomode::setItemText); false: not taken (the box shows the
            // setting again).
            std::function<bool(const plus::photomode::Item&, const std::string&)> writeText;
            // Makes the look list's row (composite "look", stepCombo() is meant for it). Null: no look row.
            std::function<void(Stack&, const plus::photomode::Item&)> look;
            // The typed text's grey text while it is empty (its tip is the item's note).
            std::string textHint;
        };

        // content: the panel inside the scroll container (emptied first); width: its width.
        Stack(LLPanel* content, S32 width, std::vector<Row>& rows);

        // ---- how the rows are made (before the first row) -----------------------------------------
        // true: the label beside its control on one line (slider 31, check 30, label and drop-down 33);
        // false (the default): the label on a line over the control.
        void setLabelsBeside(bool beside) { mBeside = beside; }
        // The marks at the left of each value row, in a strip width pixels wide (the rows move right by it).
        void setDecorator(Decorator decorator, S32 width);
        void setItemHooks(ItemHooks hooks) { mHooks = std::move(hooks); }

        // ---- headings and text ---------------------------------------------------------------------
        // A heading; true when its section is open (its rows are to be added). toggled: called on a click
        // (the column is made again later: the heading is deleted then).
        bool header(const Heading& heading, bool open, std::function<void()> toggled);
        // A sub-heading (size sub): a line over it but for the first thing of the column.
        PsHeader* subHeader(const std::string& label, const std::string& tip = std::string());
        // A sub-heading that folds the rows after it, up to endFold(), without making them again (the photo
        // tools' "Details (N)"): a click opens or closes it and lays the column out again. changed: after
        // that (null: nothing). One fold at a time (folds are not nested).
        PsHeader* foldHeader(const std::string& label, bool open, std::function<void(bool open)> changed = nullptr);
        void endFold();
        void label(const std::string& text, const std::string& tip = std::string());
        void gap(S32 pixels);
        void endSection();

        // ---- value rows ------------------------------------------------------------------------------
        PsCheck*  check(const std::string& label, const std::string& tip, std::function<float()> get,
                        std::function<void(float)> set);
        // A slider under its label, or with it (no label: only the slider). default_value: the same
        // unscaled units as min/max/step (this scales it too), shown as a "D" button that resets to
        // it; absent (the default): no button.
        PsSlider* slider(const std::string& label, const std::string& tip, float min, float max, float step,
                         int digits, float scale, std::function<float()> get, std::function<void(float)> set,
                         S32 ticks = 0, std::optional<float> default_value = std::nullopt);
        PsCombo*  combo(const std::string& label, const std::string& tip, const std::vector<plus::photomode::Option>& options,
                        std::function<float()> get, std::function<void(float)> set);
        // A choice of two or more on one bar (plus_ps_segment); the value is the index of the choice.
        PsSegment* segment(const std::string& label, const std::string& tip, const std::vector<std::string>& choices,
                           std::function<float()> get, std::function<void(float)> set);
        // A box to type in (the official line editor in a glass box): set returns false for a text it does
        // not take (the box shows get() again). hint: the grey text while it is empty.
        LLLineEditor* text(const std::string& label, const std::string& tip, const std::string& hint,
                           std::function<std::string()> get, std::function<bool(const std::string&)> set);

        // ---- rows the caller fills -------------------------------------------------------------------
        // A drop-down under its label (or beside it) that is not a row: the caller fills it and gives the
        // commit callback (choices that are not numbers: people, skies).
        PsCombo*  plainCombo(const std::string& label, const std::string& tip);
        // plainCombo() with an arrow button on each side that steps the choice (the look list): step is
        // called with -1 or 1. The caller fills the drop-down, commits and reads it back.
        PsCombo*  stepCombo(const std::string& label, const std::string& tip, const std::string& prev_tip,
                            const std::string& next_tip, std::function<void(int step)> step);
        PsButton* button(const std::string& label, const std::string& tip, std::function<void()> action);
        PsCard*   card(const char* kind, const char* icon, const std::string& title, const std::string& subtitle,
                       const std::string& tip, std::function<void()> action);
        PsPicture* picture(const char* placeholder, S32 height);
        LLTextBox* note(const std::string& text);
        // Preset cards two to a line (the last line may have one).
        PsPresetCard* presetCard(const std::string& label, const std::string& subtitle, const char* icon,
                                 bool chosen, const std::string& tip, std::function<void()> action);

        // A view made by the caller, as wide as the rows (innerWidth()), height high, with gap pixels over
        // and under it. The view is made with that width and any height; it is placed by finish().
        void view(LLView* view, S32 height, S32 gap = 0);
        S32  innerWidth() const { return mRight - stack::PAD; }

        // The photo tools' item as the row its kind asks for (false: not shown here: a typed text or the
        // look list without its hook).
        bool item(const plus::photomode::Item& item);
        bool itemById(const char* id);

        // Places what was added; the content panel gets the height of the column (returned).
        S32 finish();

        // The rows from the values now (each widget written only when its value changed).
        static void refresh(std::vector<Row>& rows);

    private:
        // What finish() placed, kept while the column's widgets live (a fold's heading holds it): a fold
        // opened or closed lays the column out again from it.
        struct Layout;
        struct Line
        {
            S32 top, height;    // the row's first line, from the column's top
        };

        // A view at its place (top: from the column's top), in the fold being made; place() also moves down.
        void put(LLView* view, S32 left, S32 top, S32 width, S32 height);
        void place(LLView* view, S32 left, S32 width, S32 height);
        void flushPresetLine();
        Row& addRow(LLUICtrl* ctl, Row::Type type, float scale, std::function<float()> get);
        // The row of a value control (null for none): its commit writes the value and has the row read back what
        // the setting took on the next tick.
        Row* addValueRow(LLUICtrl* ctl, Row::Type type, float scale, std::function<float()> get,
                         std::function<void(LLUICtrl*)> write);
        // A small label on a line of its own, from left to the rows' right.
        void labelLine(S32 left, const std::string& text, const std::string& tip);
        PsHeader* subHeading(const char* name, const std::string& label, const std::string& tip, bool collapsible,
                             bool open);
        // A drop-down under or beside its label (not placed yet: placeControl()).
        PsCombo* comboRow(const std::string& label, const std::string& tip, S32& left, Line& line, PsLabel*& beside);
        // A control of a row with a label (COMBO_H high, COMBO_PAD under it) from left to the rows' right.
        void placeControl(LLView* view, S32 left);
        // The left of a value row's control and its width (after the decoration strip).
        S32  rowLeft() const;
        S32  rowWidth() const { return mRight - rowLeft(); }
        // The label of a row made beside its control (as wide as the right panel's), or null.
        PsLabel* besideLabel(const std::string& label, const std::string& tip, S32 height);
        // The label of a row whose control is height high, pad over and under it: on a line over the
        // control, or beside it. Sets left to where the control goes and beside to the label beside it (or
        // null); returns the row's first line (the marks are centred on it).
        Line rowLabel(const std::string& label, const std::string& tip, S32 height, S32 pad, S32& left,
                      PsLabel*& beside);
        // The marks of the row whose first line is line (nothing without a decorator).
        void decorate(const Row* row, const Line& line);
        Row* lastRowSince(size_t first);

        LLPanel* mContent;
        S32 mWidth;
        S32 mRight;                 // the right edge of the rows
        S32 mY = 0;                 // from the top
        std::vector<Row>& mRows;
        std::shared_ptr<Layout> mLayout;
        S32 mPresetColumn = 0;      // the next preset card's column (0, 1)
        bool mBeside = false;
        Decorator mDecorator;
        S32 mDecorWidth = 0;
        ItemHooks mHooks;
        const plus::photomode::Item* mItem = nullptr;   // the item whose row item() is making
    };

    // Sub-tabs over pages: the tabs (plus_ps_tab, or any control whose value is "chosen") and the pages
    // (panels) in the same order; a click on a tab shows its page and hides the others. It only shows and
    // hides: what a page holds, when it is made and what is remembered are the caller's. Copies share the
    // same tabs; the tabs' callbacks keep what they need alive (a TabPages may go before its tabs).
    // For the pages of a code-built column (the photo tools, the parts sample). Pages laid out in an XUI file,
    // and pages the preferences' search must be able to open, are a plus_ps_tabs (pstabs.h: the official tab
    // container, which the search knows) instead; both move the keyboard focus out of a page they hide in
    // the same way (releaseFocusIn(), pswidgets.h).
    class TabPages
    {
    public:
        TabPages();

        // The next tab and its page (index count() before the call). A null one is kept as a hole (its
        // index is not shown).
        void add(LLUICtrl* tab, LLView* page);
        // Finds the tab and the page named in parent (recursively); a missing pair is a hole and warned about.
        void add(LLView* parent, const std::string& tab, const std::string& page);

        // Called after a page is shown: by a click on its tab (picked true) or by show() (picked false).
        void setShownCallback(std::function<void(S32 index, bool picked)> shown);

        // Shows the page of index (clamped) and chooses its tab; the others are hidden and not chosen.
        void show(S32 index);
        S32  shown() const;
        S32  count() const;
        LLView* page(S32 index) const;
        // The index of the page view is on (view or one of its ancestors), or -1.
        S32  pageOf(const LLView* view) const;

    private:
        struct Shared;
        std::shared_ptr<Shared> mShared;
    };
}

#endif // PLUS_PSSTACK_H
