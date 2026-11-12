/**
 * @file psstack.cpp
 * @brief A column of psui widgets stacked from the top, the rows that follow values, and the sub-tabs
 *        that show one page each (research/221 ps-left, research/265 pt-ui-parts).
 *
 * The column is made again only when what it shows changes (never per frame). A tick of a quarter of a
 * second reads the values of the rows (Stack::refresh()); a widget is written only when its value changed,
 * and a slider or a typed text not while it is dragged or typed in. A fold opened or closed only moves and
 * hides the widgets that are there.
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

#include "psstack.h"

#include "psstyle.h"
#include "pswidgets.h"

#include <cmath>

#include "llcombobox.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

namespace plus::psui
{
    namespace pm = plus::photomode;
    using namespace stack;

    namespace
    {
        // A widget's params from the registry's defaults (the drop-down's come from widgets/plus_ps_combo.xml).
        template <class T>
        typename T::Params paramsOf(const char* name, S32 width, S32 height, const std::string& label,
                                    const std::string& tip)
        {
            typename T::Params p(LLUICtrlFactory::getDefaultParams<T>());
            p.name = name;
            p.rect = LLRect(0, height, width, 0);
            p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
            if (!label.empty())
            {
                p.label = label;
            }
            if (!tip.empty())
            {
                p.tool_tip = tip;
            }
            return p;
        }

        std::string tipOf(const std::string& label, const std::string& tip)
        {
            return tip.empty() ? std::string() : label + "\n" + tip;
        }
    }

    void setComboIfChanged(LLComboBox* combo, const LLSD& value)
    {
        const LLSD current = combo->getValue();
        if (current.type() != value.type() || current.asString() != value.asString())
        {
            combo->setValue(value);
        }
    }

    LLTextBox* makeNote(const std::string& name, const LLRect& rect, const std::string& text, const LLFontGL* font,
                        const LLColor4& color, U32 follows)
    {
        LLTextBox::Params p;
        p.name          = name;
        p.rect          = rect;
        p.follows.flags = follows;
        p.initial_value = text;
        p.wrap          = true;
        p.font          = font;
        p.text_color    = color;
        p.mouse_opaque  = false;
        return LLUICtrlFactory::create<LLTextBox>(p);
    }

    PsIconButton* stack::makeStar(const std::string& tip, bool starred)
    {
        PsIconButton::Params p = paramsOf<PsIconButton>("star", DECOR_STAR_W, DECOR_STAR_W, std::string(), tip);
        p.star          = true;
        p.toggle        = true;
        p.initial_value = starred;
        return LLUICtrlFactory::create<PsIconButton>(p);
    }

    PsLabel* stack::makeMark(const std::string& text, const std::string& tip)
    {
        PsLabel::Params p = paramsOf<PsLabel>("mark", DECOR_MARK_W, LABEL_H, text, tip);
        p.kind  = "hint";
        p.align = "center";
        if (!tip.empty())
        {
            p.mouse_opaque = true;  // for its tool tip
        }
        return LLUICtrlFactory::create<PsLabel>(p);
    }

    // =========================================================================================
    // The layout kept for the folds

    struct Stack::Layout
    {
        struct Placed
        {
            LLHandle<LLView> view;
            S32 left, top, width, height;   // top: from the column's top, every fold open
            S32 fold;                       // the fold it is in (-1: none)
        };
        // A fold's widgets are in a see-through panel of their own: closing the fold hides that panel only,
        // so what the caller shows or hides inside it (a "changed" mark) stays as the caller left it.
        struct Fold
        {
            S32  begin = 0, end = 0;        // the part of the column it takes while open
            bool open = true;
            LLHandle<LLPanel> panel;
            bool added = false;
        };

        LLHandle<LLPanel>   content;
        S32                 width = 0;
        S32                 height = 0;     // every fold open
        std::vector<Placed> placed;
        std::vector<Fold>   folds;
        S32                 building = -1;  // the fold being added to (while the column is made)

        // How much the closed folds over y take away.
        S32 closedAbove(S32 y) const
        {
            S32 removed = 0;
            for (const Fold& fold : folds)
            {
                if (!fold.open && fold.end <= y)
                {
                    removed += fold.end - fold.begin;
                }
            }
            return removed;
        }

        void placeFold(Fold& fold, LLPanel* parent, S32 h, bool add)
        {
            LLPanel* panel = fold.panel.get();
            if (!panel)
            {
                return;
            }
            const S32 top = fold.begin - closedAbove(fold.begin);
            panel->setRect(LLRect(0, h - top, width, h - top - (fold.end - fold.begin)));
            panel->setVisible(fold.open);
            if (add && !fold.added)
            {
                fold.added = true;
                parent->addChild(panel);
            }
        }

        // Sizes the content and puts every widget in its place (add: adds them to the content, or to their
        // fold's panel, the first time). Returns the content's height.
        S32 apply(LLPanel* parent, bool add)
        {
            if (!parent)
            {
                return 0;
            }
            const S32 h = llmax(height - closedAbove(height), 1);
            parent->reshape(width, h);
            for (const Placed& p : placed)
            {
                LLView* view = p.view.get();
                if (!view)
                {
                    continue;
                }
                if (p.fold < 0)
                {
                    const S32 top = p.top - closedAbove(p.top);
                    view->setRect(LLRect(p.left, h - top, p.left + p.width, h - top - p.height));
                    if (add)
                    {
                        parent->addChild(view);
                    }
                    continue;
                }
                // inside its fold's panel: placed once, the panel moves
                Fold& fold = folds[p.fold];
                if (add && !fold.added)
                {
                    placeFold(fold, parent, h, true);   // in the content's order of children (and of Tab)
                }
                if (add)
                {
                    LLPanel* panel = fold.panel.get();
                    const S32 fh  = fold.end - fold.begin;
                    const S32 top = p.top - fold.begin;
                    view->setRect(LLRect(p.left, fh - top, p.left + p.width, fh - top - p.height));
                    if (panel)
                    {
                        panel->addChild(view);
                    }
                }
            }
            for (Fold& fold : folds)
            {
                placeFold(fold, parent, h, add);    // the empty ones, and every one again after a click
            }
            return h;
        }
    };

    // =========================================================================================
    // Stack

    Stack::Stack(LLPanel* content, S32 width, std::vector<Row>& rows)
    :   mContent(content),
        mWidth(width),
        mRows(rows),
        mLayout(std::make_shared<Layout>())
    {
        static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
        // The widgets go with the content's children; the rows after them, as a widget going away may still
        // reach its row (a value being typed, committed as its box loses the focus).
        mContent->deleteAllChildren();
        mRows.clear();
        mRight = mWidth - llmax(PAD_RIGHT_MIN, PAD - (S32)scrollbar_size);
        if (mRight - PAD < 60)
        {
            mRight = mWidth;    // a panel too narrow for the scroll bar's room
        }
        mLayout->content = mContent->getHandle();
        mLayout->width   = mWidth;
    }

    void Stack::setDecorator(Decorator decorator, S32 width)
    {
        mDecorator  = std::move(decorator);
        mDecorWidth = mDecorator ? llmax(0, width) : 0;
    }

    S32 Stack::rowLeft() const
    {
        return PAD + mDecorWidth;
    }

    void Stack::put(LLView* view, S32 left, S32 top, S32 width, S32 height)
    {
        if (view)
        {
            mLayout->placed.push_back({ view->getHandle(), left, top, width, height, mLayout->building });
        }
    }

    void Stack::place(LLView* view, S32 left, S32 width, S32 height)
    {
        put(view, left, mY, width, height);
        mY += height;
    }

    void Stack::flushPresetLine()
    {
        if (mPresetColumn != 0)
        {
            mPresetColumn = 0;
            mY += PRESET_H + PRESET_GAP;
        }
    }

    Stack::Row& Stack::addRow(LLUICtrl* ctl, Row::Type type, float scale, std::function<float()> get)
    {
        Row row;
        row.ctl   = ctl;
        row.type  = type;
        row.scale = scale;
        row.get   = std::move(get);
        row.item  = mItem;
        mRows.push_back(std::move(row));
        return mRows.back();
    }

    Stack::Row* Stack::addValueRow(LLUICtrl* ctl, Row::Type type, float scale, std::function<float()> get,
                                   std::function<void(LLUICtrl*)> write)
    {
        if (!ctl)
        {
            return nullptr;
        }
        const size_t index = mRows.size();
        std::vector<Row>* rows = &mRows;
        ctl->setCommitCallback([write = std::move(write), rows, index](LLUICtrl* c, const LLSD&) {
            write(c);
            (*rows)[index].shown_set = false;   // read back what the setting took
        });
        return &addRow(ctl, type, scale, std::move(get));
    }

    Stack::Row* Stack::lastRowSince(size_t first)
    {
        return mRows.size() > first ? &mRows.back() : nullptr;
    }

    void Stack::labelLine(S32 left, const std::string& text, const std::string& tip)
    {
        mY += LABEL_TOP;
        PsLabel::Params p = paramsOf<PsLabel>("label", mRight - left, LABEL_H, text, tip);
        p.kind = "label";
        place(LLUICtrlFactory::create<PsLabel>(p), left, mRight - left, LABEL_H);
    }

    PsLabel* Stack::besideLabel(const std::string& label, const std::string& tip, S32 height)
    {
        PsLabel::Params p = paramsOf<PsLabel>("label", style::SLIDER_LABEL_WIDTH, height, label, tip);
        p.kind = "label";
        if (!tip.empty())
        {
            p.mouse_opaque = true;  // for its tool tip
        }
        return LLUICtrlFactory::create<PsLabel>(p);
    }

    void Stack::decorate(const Row* row, const Line& line)
    {
        if (!mDecorator || mDecorWidth <= 0)
        {
            return;
        }
        std::vector<LLView*> views;
        mDecorator(mItem, row ? S32(row - mRows.data()) : -1, views);
        S32 x = PAD;
        for (LLView* view : views)
        {
            if (!view)
            {
                continue;
            }
            const S32 w = view->getRect().getWidth();
            const S32 h = llmin(view->getRect().getHeight(), line.height);
            put(view, x, line.top + (line.height - h) / 2, w, h);
            x += w;
        }
    }

    // ---- headings and text ------------------------------------------------------------------------

    bool Stack::header(const Heading& heading, bool open, std::function<void()> toggled)
    {
        flushPresetLine();
        PsHeader::Params p = paramsOf<PsHeader>("heading", mWidth, HEADER_H, heading.label, std::string());
        p.size        = "panel";
        p.divider     = mY > 0 ? "top" : "none";
        p.collapsible = true;
        p.initial_value = open;
        if (heading.icon)
        {
            p.icon = heading.icon;
        }
        PsHeader* view = LLUICtrlFactory::create<PsHeader>(p);
        if (view)
        {
            view->setCommitCallback([toggled](LLUICtrl*, const LLSD&) { toggled(); });
        }
        place(view, 0, mWidth, HEADER_H);
        return open;
    }

    PsHeader* Stack::subHeading(const char* name, const std::string& label, const std::string& tip, bool collapsible,
                                bool open)
    {
        flushPresetLine();
        PsHeader::Params p = paramsOf<PsHeader>(name, mRight - PAD, SUB_H, label, tip);
        p.size        = "sub";
        p.divider     = mY > 0 ? "top" : "none";
        p.collapsible = collapsible;
        p.initial_value = open;
        PsHeader* view = LLUICtrlFactory::create<PsHeader>(p);
        place(view, PAD, mRight - PAD, SUB_H);
        return view;
    }

    PsHeader* Stack::subHeader(const std::string& label, const std::string& tip)
    {
        return subHeading("sub_heading", label, tip, false, true);
    }

    PsHeader* Stack::foldHeader(const std::string& label, bool open, std::function<void(bool)> changed)
    {
        endFold();      // folds are not nested
        PsHeader* view = subHeading("fold_heading", label, std::string(), true, open);

        const S32 fold = (S32)mLayout->folds.size();
        Layout::Fold entry;
        entry.begin = entry.end = mY;
        entry.open  = open;
        LLPanel::Params fp;
        fp.name               = "fold";
        fp.rect               = LLRect(0, 1, mWidth, 0);
        fp.follows.flags      = FOLLOWS_LEFT | FOLLOWS_TOP;
        fp.background_visible = false;
        fp.mouse_opaque       = false;
        if (LLPanel* panel = LLUICtrlFactory::create<LLPanel>(fp))
        {
            entry.panel = panel->getHandle();
        }
        mLayout->folds.push_back(entry);
        mLayout->building = fold;
        if (view)
        {
            // The layout lives as long as this heading (the column's widgets); nothing is made or deleted here,
            // so this may run from the heading's own click.
            std::shared_ptr<Layout> layout = mLayout;
            view->setCommitCallback([layout, fold, changed](LLUICtrl* ctrl, const LLSD&) {
                const bool now = ctrl->getValue().asBoolean();
                if (!now)
                {
                    releaseFocusIn(layout->folds[fold].panel.get(), ctrl);  // to the heading clicked
                }
                layout->folds[fold].open = now;
                layout->apply(layout->content.get(), false);
                if (changed)
                {
                    changed(now);
                }
            });
        }
        return view;
    }

    void Stack::endFold()
    {
        if (mLayout->building < 0)
        {
            return;
        }
        flushPresetLine();
        mLayout->folds[mLayout->building].end = mY;
        mLayout->building = -1;
    }

    void Stack::label(const std::string& text, const std::string& tip)
    {
        flushPresetLine();
        labelLine(PAD, text, tip);
    }

    void Stack::gap(S32 pixels)
    {
        flushPresetLine();
        mY += pixels;
    }

    void Stack::endSection()
    {
        flushPresetLine();
        mY += SECTION_END;
    }

    // ---- value rows ---------------------------------------------------------------------------------

    Stack::Line Stack::rowLabel(const std::string& label, const std::string& tip, S32 height, S32 pad, S32& left,
                                PsLabel*& beside)
    {
        flushPresetLine();
        left   = rowLeft();
        beside = nullptr;
        if (!mBeside)
        {
            if (label.empty())
            {
                mY += pad;
                return { mY, height };
            }
            labelLine(left, label, tip);
            mY += pad;
            return { mY - pad - LABEL_H, LABEL_H };
        }
        mY += pad;
        if (!label.empty())
        {
            beside = besideLabel(label, tip, height);
            put(beside, left, mY, style::SLIDER_LABEL_WIDTH, height);
            left += style::SLIDER_LABEL_WIDTH + style::SLIDER_GAP;
        }
        return { mY, height };
    }

    void Stack::placeControl(LLView* view, S32 left)
    {
        place(view, left, mRight - left, COMBO_H);
        mY += COMBO_PAD;
    }

    PsCheck* Stack::check(const std::string& label, const std::string& tip, std::function<float()> get,
                          std::function<void(float)> set)
    {
        flushPresetLine();
        const Line line { mY, CHECK_H };
        PsCheck::Params p = paramsOf<PsCheck>("check", rowWidth(), CHECK_H, label, tipOf(label, tip));
        PsCheck* view = LLUICtrlFactory::create<PsCheck>(p);
        const Row* row = addValueRow(view, Row::Type::Check, 1.f, std::move(get),
                                     [set](LLUICtrl* c) { set(c->getValue().asBoolean() ? 1.f : 0.f); });
        place(view, rowLeft(), rowWidth(), CHECK_H);
        decorate(row, line);
        return view;
    }

    PsSlider* Stack::slider(const std::string& label, const std::string& tip, float min, float max, float step,
                            int digits, float scale, std::function<float()> get, std::function<void(float)> set,
                            S32 ticks, std::optional<float> default_value)
    {
        // Beside: the slider draws its own label (one line, the right panel's); over: a label line first.
        S32 left = rowLeft();
        Line line {};
        if (mBeside)
        {
            flushPresetLine();
            line = { mY, LINE_SLIDER_H };
        }
        else
        {
            PsLabel* beside = nullptr;      // none over a line of its own
            line = rowLabel(label, tipOf(label, tip), SLIDER_H, 0, left, beside);
        }
        const S32 height = mBeside ? LINE_SLIDER_H : SLIDER_H;
        PsSlider::Params p = paramsOf<PsSlider>("slider", mRight - left, height, mBeside ? label : std::string(),
                                                tipOf(label, tip));
        p.min_value      = min * scale;
        p.max_value      = max * scale;
        p.increment      = step * scale;
        p.decimal_digits = digits;
        if (!mBeside || label.empty())
        {
            p.label_width = 0;
        }
        p.ticks          = ticks;
        if (default_value)
        {
            // the same unscaled units as min/max/step above
            p.default_value = *default_value * scale;
        }
        PsSlider* view = LLUICtrlFactory::create<PsSlider>(p);
        const Row* row = addValueRow(view, Row::Type::Slider, scale, std::move(get),
                                     [set, scale](LLUICtrl* c) { set(float(c->getValue().asReal()) / scale); });
        place(view, left, mRight - left, height);
        decorate(row, line);
        return view;
    }

    PsCombo* Stack::comboRow(const std::string& label, const std::string& tip, S32& left, Line& line, PsLabel*& beside)
    {
        line = rowLabel(label, tipOf(label, tip), COMBO_H, COMBO_PAD, left, beside);
        PsCombo::Params p = paramsOf<PsCombo>("combo", mRight - left, COMBO_H, std::string(), tipOf(label, tip));
        p.commit_on_arrow = true;   // the photo screen's drop-downs choose with the arrow keys at once (combo-arrow)
        return LLUICtrlFactory::create<PsCombo>(p);
    }

    PsCombo* Stack::combo(const std::string& label, const std::string& tip, const std::vector<pm::Option>& options,
                          std::function<float()> get, std::function<void(float)> set)
    {
        S32 left = 0;
        Line line {};
        PsLabel* beside = nullptr;
        PsCombo* view = comboRow(label, tip, left, line, beside);
        if (view)
        {
            for (const pm::Option& option : options)
            {
                view->add(option.label, LLSD((LLSD::Integer)ll_round(option.value)), ADD_BOTTOM, !option.heading);
            }
        }
        Row* row = addValueRow(view, Row::Type::Combo, 1.f, std::move(get),
                               [set](LLUICtrl* c) { set(float(c->getValue().asReal())); });
        if (row)
        {
            row->options = options;
            row->label   = beside;
        }
        placeControl(view, left);
        decorate(row, line);
        return view;
    }

    PsSegment* Stack::segment(const std::string& label, const std::string& tip, const std::vector<std::string>& choices,
                              std::function<float()> get, std::function<void(float)> set)
    {
        S32 left = 0;
        PsLabel* beside = nullptr;
        const Line line = rowLabel(label, tipOf(label, tip), COMBO_H, COMBO_PAD, left, beside);
        PsSegment::Params p = paramsOf<PsSegment>("segment", mRight - left, COMBO_H, std::string(), tipOf(label, tip));
        PsSegment* view = LLUICtrlFactory::create<PsSegment>(p);
        if (view)
        {
            view->setChoices(choices);
        }
        Row* row = addValueRow(view, Row::Type::Segment, 1.f, std::move(get),
                               [set](LLUICtrl* c) { set(float(c->getValue().asInteger())); });
        if (row)
        {
            row->label = beside;
        }
        placeControl(view, left);
        decorate(row, line);
        return view;
    }

    LLLineEditor* Stack::text(const std::string& label, const std::string& tip, const std::string& hint,
                              std::function<std::string()> get, std::function<bool(const std::string&)> set)
    {
        S32 left = 0;
        PsLabel* beside = nullptr;
        const Line line = rowLabel(label, tipOf(label, tip), COMBO_H, COMBO_PAD, left, beside);
        const S32 width = mRight - left;

        // The official line editor, see-through, in a glass box; its colours from the style table.
        PsBox::Params b(LLUICtrlFactory::getDefaultParams<PsBox>());
        b.name          = "text_box";
        b.rect          = LLRect(0, COMBO_H, width, 0);
        b.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
        b.kind          = "glass";
        b.mouse_opaque  = false;
        PsBox* box = LLUICtrlFactory::create<PsBox>(b);

        LLLineEditor* editor = nullptr;
        Row* row = nullptr;
        if (box)
        {
            LLLineEditor::Params p(LLUICtrlFactory::getDefaultParams<LLLineEditor>());
            p.name                 = "text";
            p.rect                 = LLRect(TEXT_INSET, COMBO_H - TEXT_INSET, llmax(TEXT_INSET + 1, width - TEXT_INSET), TEXT_INSET);
            p.follows.flags        = FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_TOP;
            p.max_length.chars     = 64;
            p.commit_on_focus_lost = true;      // a setting: what was typed is taken as the box is left (as before)
            p.revert_on_esc        = true;
            p.select_on_focus      = true;
            p.use_bg_color         = true;
            p.bg_color             = LLColor4::transparent;
            p.text_color           = style::TEXT_WHITE;
            p.cursor_color         = style::TEXT_WHITE;
            p.text_readonly_color  = style::TEXT_DIM;
            p.text_tentative_color = style::TEXT_DIM;
            p.draw_focus_border    = false;
            p.border.visible       = false;
            p.text_pad_left        = 6;
            if (!hint.empty())
            {
                p.label = hint;
            }
            const std::string row_tip = tipOf(label, tip);
            if (!row_tip.empty())
            {
                p.tool_tip = row_tip;
            }
            editor = LLUICtrlFactory::create<LLLineEditor>(p);
            if (editor)
            {
                box->addChild(editor);
            }
            row = addValueRow(editor, Row::Type::Text, 1.f, nullptr, [get, set](LLUICtrl* c) {
                if (!set(c->getValue().asString()))
                {
                    static_cast<LLLineEditor*>(c)->setText(get());     // not taken: the setting again
                }
            });
            if (row)
            {
                row->text  = std::move(get);
                row->label = beside;
            }
        }
        placeControl(box, left);
        decorate(row, line);
        return editor;
    }

    // ---- rows the caller fills ----------------------------------------------------------------------

    PsCombo* Stack::plainCombo(const std::string& label, const std::string& tip)
    {
        S32 left = 0;
        Line line {};
        PsLabel* beside = nullptr;
        PsCombo* view = comboRow(label, tip, left, line, beside);
        placeControl(view, left);
        return view;
    }

    PsCombo* Stack::stepCombo(const std::string& label, const std::string& tip, const std::string& prev_tip,
                              const std::string& next_tip, std::function<void(int)> step)
    {
        S32 left = 0;
        PsLabel* beside = nullptr;
        const Line line = rowLabel(label, tipOf(label, tip), COMBO_H, COMBO_PAD, left, beside);
        const S32 top = mY;
        auto arrow = [this, &step, top](const char* name, const char* icon, const std::string& arrow_tip, int dir, S32 x) {
            PsIconButton::Params p = paramsOf<PsIconButton>(name, STEP_W, COMBO_H, std::string(), arrow_tip);
            p.icon = icon;
            PsIconButton* button = LLUICtrlFactory::create<PsIconButton>(p);
            if (button && step)
            {
                button->setCommitCallback([step, dir](LLUICtrl*, const LLSD&) { step(dir); });
            }
            put(button, x, top, STEP_W, COMBO_H);
        };
        // in the order of Tab: the arrow before, the drop-down, the arrow after
        const S32 combo_left  = left + STEP_W + STEP_GAP;
        const S32 combo_width = llmax(1, mRight - STEP_W - STEP_GAP - combo_left);
        arrow("step_prev", "chevron_left", prev_tip, -1, left);
        PsCombo::Params p = paramsOf<PsCombo>("combo", combo_width, COMBO_H, std::string(), tipOf(label, tip));
        p.commit_on_arrow = true;   // as comboRow()
        PsCombo* view = LLUICtrlFactory::create<PsCombo>(p);
        put(view, combo_left, top, combo_width, COMBO_H);
        arrow("step_next", "chevron_right", next_tip, 1, mRight - STEP_W);
        mY = top + COMBO_H + COMBO_PAD;
        decorate(nullptr, line);
        return view;
    }

    PsButton* Stack::button(const std::string& label, const std::string& tip, std::function<void()> action)
    {
        flushPresetLine();
        mY += ROW_GAP;
        PsButton::Params p = paramsOf<PsButton>("button", mRight - PAD, BUTTON_H, label, tip);
        PsButton* view = LLUICtrlFactory::create<PsButton>(p);
        if (view)
        {
            view->setCommitCallback([action](LLUICtrl*, const LLSD&) { action(); });
        }
        place(view, PAD, mRight - PAD, BUTTON_H);
        mY += ROW_GAP;
        return view;
    }

    PsCard* Stack::card(const char* kind, const char* icon, const std::string& title, const std::string& subtitle,
                        const std::string& tip, std::function<void()> action)
    {
        flushPresetLine();
        const bool avatar = std::string(kind) == "avatar";
        const S32 height = avatar ? AVATAR_H : CARD_H;
        PsCard::Params p = paramsOf<PsCard>("card", mRight - PAD, height, title, tip);
        p.kind     = kind;
        p.subtitle = subtitle;
        p.more     = !avatar && action != nullptr;
        if (icon)
        {
            p.icon = icon;
        }
        if (!action)
        {
            p.tab_stop     = false;
            p.mouse_opaque = false;     // nothing to click: no hover either
        }
        PsCard* view = LLUICtrlFactory::create<PsCard>(p);
        if (view && action)
        {
            view->setCommitCallback([action](LLUICtrl*, const LLSD&) { action(); });
        }
        mY += 4;
        place(view, PAD, mRight - PAD, height);
        mY += 4;
        return view;
    }

    PsPicture* Stack::picture(const char* placeholder, S32 height)
    {
        flushPresetLine();
        PsPicture::Params p = paramsOf<PsPicture>("picture", mRight - PAD, height, std::string(), std::string());
        p.placeholder  = placeholder;
        p.mouse_opaque = false;
        PsPicture* view = LLUICtrlFactory::create<PsPicture>(p);
        mY += 4;
        place(view, PAD, mRight - PAD, height);
        mY += 4;
        return view;
    }

    LLTextBox* Stack::note(const std::string& text)
    {
        flushPresetLine();
        LLTextBox* view = makeNote("note", LLRect(0, NOTE_H, mRight - PAD, 0), text, font(style::FONT_SMALL), style::TEXT_SUB);
        mY += 4;
        place(view, PAD, mRight - PAD, NOTE_H);
        return view;
    }

    PsPresetCard* Stack::presetCard(const std::string& label, const std::string& subtitle, const char* icon,
                                    bool chosen, const std::string& tip, std::function<void()> action)
    {
        const S32 inner = mRight - PAD;
        const S32 width = (inner - PRESET_GAP) / 2;
        PsPresetCard::Params p = paramsOf<PsPresetCard>("preset", width, PRESET_H, label, tip);
        p.subtitle = subtitle;
        p.initial_value = chosen;
        if (icon)
        {
            p.icon = icon;
        }
        PsPresetCard* view = LLUICtrlFactory::create<PsPresetCard>(p);
        if (view)
        {
            view->setValue(chosen);
            view->setCommitCallback([action](LLUICtrl*, const LLSD&) { action(); });
            put(view, PAD + mPresetColumn * (width + PRESET_GAP), mY, width, PRESET_H);
        }
        if (++mPresetColumn == 2)
        {
            flushPresetLine();
        }
        return view;
    }

    void Stack::view(LLView* view, S32 height, S32 gap)
    {
        flushPresetLine();
        mY += gap;
        place(view, PAD, mRight - PAD, height);
        mY += gap;
    }

    // ---- the photo tools' items ---------------------------------------------------------------------

    bool Stack::item(const pm::Item& item)
    {
        if (item.composite == "look")
        {
            if (!mHooks.look)
            {
                return false;   // the look is the right panel's (the photo screen)
            }
            mItem = &item;
            mHooks.look(*this, item);
            mItem = nullptr;
            return true;
        }
        const pm::Item* it = &item;     // items() lives as long as the process
        const size_t first = mRows.size();
        mItem = it;
        if (item.kind == pm::Kind::Text)
        {
            if (!mHooks.writeText)
            {
                mItem = nullptr;
                return false;   // a typed text: the photo tools have it
            }
            auto write = mHooks.writeText;
            text(item.label, item.note, mHooks.textHint, [it]() { return pm::itemText(*it); },
                 [it, write](const std::string& typed) { return write(*it, typed); });
        }
        else
        {
            auto get = [it]() { return pm::itemValue(*it); };
            std::function<void(float)> set;
            if (mHooks.write)
            {
                auto write = mHooks.write;
                set = [it, write](float v) { write(*it, v); };
            }
            else
            {
                set = [it](float v) { pm::setItemValue(*it, v); };
            }
            switch (item.kind)
            {
                case pm::Kind::Check:
                    check(item.label, item.note, get, set);
                    break;
                case pm::Kind::Combo:
                    combo(item.label, item.note, item.options, get, set);
                    break;
                default:
                    slider(item.label, item.note, item.min, item.max, item.step, item.digits, item.scale, get, set,
                           0, pm::itemDefaultValue(item));
                    break;
            }
        }
        mItem = nullptr;
        if (Row* row = lastRowSince(first))
        {
            if (!item.needs.empty())
            {
                row->needs       = gSavedSettings.getControl(item.needs);
                row->needs_value = item.needs_value;
            }
            row->blur = item.param == pm::Param::Blur;
            // the widget's tip is tipOf(label, note): the name, then the note
            row->feature.feature = item.feature;
            row->feature.tip     = item.note;
            row->feature.head    = item.label;
        }
        return true;
    }

    bool Stack::itemById(const char* id)
    {
        const pm::Item* found = pm::findItem(id);
        return found && item(*found);
    }

    S32 Stack::finish()
    {
        endFold();
        flushPresetLine();
        mLayout->height = llmax(mY, 1);
        return mLayout->apply(mContent, true);
    }

    // ---- the tick -------------------------------------------------------------------------------------

    namespace
    {
        // A typed text's row from its text now (not while it is typed in: what is typed is the newer).
        void showText(Stack::Row& row)
        {
            if (!row.text || gFocusMgr.childHasKeyboardFocus(row.ctl))
            {
                return;
            }
            std::string now = row.text();
            if (!row.shown_set || now != row.shown_text)
            {
                static_cast<LLLineEditor*>(row.ctl)->setText(now);
                row.shown_text = std::move(now);
                row.shown_set  = true;
            }
        }

        // A value row's widget from its value now. false: the row is left alone this tick (a slider dragged or
        // typed in: its own value is the newer).
        bool showValue(Stack::Row& row)
        {
            const float v = row.get();
            if (row.shown_set && v == row.shown)
            {
                return true;
            }
            switch (row.type)
            {
                case Stack::Row::Type::Slider:
                    if (gFocusMgr.getMouseCapture() == row.ctl || gFocusMgr.childHasKeyboardFocus(row.ctl))
                    {
                        return false;
                    }
                    row.ctl->setValue(LLSD((v > pm::UNSET ? v : 0.f) * row.scale));
                    break;
                case Stack::Row::Type::Check:
                    row.ctl->setValue(LLSD(v > 0.5f));
                    break;
                case Stack::Row::Type::Segment:
                    row.ctl->setValue(LLSD((LLSD::Integer)ll_round(v)));
                    break;
                case Stack::Row::Type::Combo:
                {
                    const pm::Option* nearest = nullptr;
                    for (const pm::Option& option : row.options)
                    {
                        if (!option.heading && (!nearest || std::fabs(option.value - v) < std::fabs(nearest->value - v)))
                        {
                            nearest = &option;
                        }
                    }
                    if (nearest)
                    {
                        setComboIfChanged(static_cast<LLComboBox*>(row.ctl), LLSD((LLSD::Integer)ll_round(nearest->value)));
                    }
                    break;
                }
                default:
                    break;
            }
            row.shown_set = true;
            row.shown     = v;
            return true;
        }

        // The row greyed out while what it needs is not there (written only when that changed).
        void showUsable(Stack::Row& row)
        {
            bool usable = pm::needsMet(row.needs, row.needs_value);
            if (row.blur)
            {
                usable = usable && pm::blurAvailable();
            }
            usable = row.feature.usable(nullptr, row.ctl) && usable;
            if (row.enabled != (usable ? 1 : 0))
            {
                row.enabled = usable ? 1 : 0;
                row.ctl->setEnabled(usable);
                if (row.label)
                {
                    row.label->setEnabled(usable);
                }
            }
        }
    }

    void Stack::refresh(std::vector<Row>& rows)
    {
        for (Row& row : rows)
        {
            if (!row.ctl)
            {
                continue;
            }
            if (row.type == Row::Type::Text)
            {
                showText(row);
            }
            else if (!row.get || !showValue(row))
            {
                continue;
            }
            showUsable(row);
        }
    }

    // =========================================================================================
    // TabPages

    struct TabPages::Shared
    {
        std::vector<LLHandle<LLUICtrl>> tabs;
        std::vector<LLHandle<LLView>>   pages;
        S32 shown = -1;
        std::function<void(S32, bool)> onShown;

        void show(S32 index, bool picked)
        {
            if (tabs.empty())
            {
                return;
            }
            shown = llclamp(index, 0, (S32)tabs.size() - 1);
            LLUICtrl* chosen = tabs[shown].get();
            for (S32 i = 0; i < (S32)tabs.size(); ++i)
            {
                if (LLUICtrl* tab = tabs[i].get())
                {
                    tab->setValue(LLSD(i == shown));
                }
                if (LLView* page = pages[i].get())
                {
                    if (i != shown)
                    {
                        releaseFocusIn(page, chosen);   // to the tab chosen
                    }
                    page->setVisible(i == shown);
                }
            }
            if (onShown)
            {
                onShown(shown, picked);
            }
        }
    };

    TabPages::TabPages()
    :   mShared(std::make_shared<Shared>())
    {
    }

    void TabPages::add(LLUICtrl* tab, LLView* page)
    {
        const S32 index = (S32)mShared->tabs.size();
        mShared->tabs.push_back(tab ? tab->getHandle() : LLHandle<LLUICtrl>());
        mShared->pages.push_back(page ? page->getHandle() : LLHandle<LLView>());
        if (tab)
        {
            // The tabs' callbacks hold the shared part: the pages are shown however long the TabPages lives.
            std::shared_ptr<Shared> shared = mShared;
            tab->setCommitCallback([shared, index](LLUICtrl*, const LLSD&) { shared->show(index, true); });
        }
    }

    void TabPages::add(LLView* parent, const std::string& tab, const std::string& page)
    {
        LLUICtrl* t = parent ? parent->findChild<LLUICtrl>(tab) : nullptr;
        LLView*   p = parent ? parent->findChildView(page) : nullptr;
        if (!t || !p)
        {
            LL_WARNS("Plus") << "TabPages: no " << tab << " or " << page << LL_ENDL;
            t = nullptr;
            p = nullptr;
        }
        add(t, p);
    }

    void TabPages::setShownCallback(std::function<void(S32, bool)> shown)
    {
        mShared->onShown = std::move(shown);
    }

    void TabPages::show(S32 index)
    {
        mShared->show(index, false);
    }

    S32 TabPages::shown() const
    {
        return mShared->shown;
    }

    S32 TabPages::count() const
    {
        return (S32)mShared->tabs.size();
    }

    LLView* TabPages::page(S32 index) const
    {
        return index >= 0 && index < count() ? mShared->pages[index].get() : nullptr;
    }

    S32 TabPages::pageOf(const LLView* view) const
    {
        for (S32 i = 0; view && i < count(); ++i)
        {
            const LLView* page = mShared->pages[i].get();
            if (page && (view == page || view->hasAncestor(page)))
            {
                return i;
            }
        }
        return -1;
    }
}
