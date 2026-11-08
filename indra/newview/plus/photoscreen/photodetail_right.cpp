/**
 * @file photodetail_right.cpp
 * @brief The right of the detailed photo screen (research/221 13., 14.): the camera settings in folding
 *        sections, drawn with the photo screen's widgets (psui/), over the shutter strip.
 *
 * The panel (panel_plus_photoscreen_right.xml) has two boxes: the sections (a scroll container) and the
 * shutter strip under them (photo / 360°, the shutter, the photo tools, "F12 to leave"). The rows of the
 * sections are made here once, from the XML's strings and the photo tools' items, and are only laid out
 * again after that: a section that folds hides its rows (nothing is deleted or made again). The values come
 * from the same functions as the photo tools' window (pm::itemValue / setItemValue, pm::value / setValue,
 * the focus lock, the look list), read every quarter of a second for the open sections only; a widget is
 * written only when its value changed.
 *
 * What the target picture has but the viewer cannot do yet is not shown (research/221 (3) "後で"): the
 * bokeh's shape, highlights / whites / blacks, temperature / tint / vibrance / saturation, a focus distance
 * set by hand (the distance is shown only), and video.
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

#include "photodetail_right.h"

#include "photodetail_rows.h"
#include "photodetail_state.h"
#include "photoscreen_items.h"
#include "psui/psstyle.h"
#include "psui/pswidgets.h"
#include "plus/photomode/photolook.h"
#include "plus/photomode/photomode.h"
#include "plus/photomode/photoshot.h"

#include <cmath>
#include <optional>

#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llnotificationsutil.h"
#include "llscrollcontainer.h"
#include "llscrolllistitem.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

namespace plus::photoscreen::detail
{
    namespace
    {
        namespace ui = plus::psui;
        namespace st = plus::psui::style;

        // --- sizes (UI pixels at a window 1080 high; research/221 13., 14.) --------------------------------
        constexpr S32 GAP          = st::PANEL_GAP;    // around the two boxes and between them
        constexpr S32 SHUTTER_H    = 134;              // the shutter strip's box
        constexpr S32 SCROLL_INSET = 6;                // the scroll inside the box, off its round corners
        constexpr S32 SCROLLBAR    = 10;               // size= of the scroll container in the XML
        constexpr S32 MARGIN       = st::ROW_MARGIN;   // left and right of the rows
        constexpr S32 LABEL_W      = st::SLIDER_LABEL_WIDTH;
        constexpr S32 HEADER_H     = 40;
        constexpr S32 SUB_H        = 30;
        constexpr S32 ROW_H        = 31;
        constexpr S32 COMBO_ROW_H  = 37;
        constexpr S32 COMBO_H      = 33;
        constexpr S32 DIVIDER_H    = 9;
        constexpr S32 BUTTON_H     = 40;
        constexpr S32 BUTTON_ROW_H = 46;
        constexpr S32 AFTER_HEADER = 6;
        constexpr S32 SECTION_END  = 12;
        constexpr S32 SWITCH_W     = 31;
        constexpr S32 ICON_BTN_W   = 35;
        constexpr S32 SIDE_GAP     = 8;                // between a label and the switch or button after it
        constexpr S32 BUTTON_GAP   = 10;

        // The f-numbers of the photo tools' background blur (photomode.h: Blur 0..100 is f/22 .. f/1.4, log).
        constexpr F32 F_WIDE   = 1.4f;
        constexpr F32 F_NARROW = 22.f;

        constexpr char SETTING_FNUMBER[] = "CameraFNumber";
        constexpr char SETTING_MAX_COF[] = "CameraMaxCoF";
        constexpr F32  MAX_COF_LIMIT     = 20.f;      // the official default is 10

        float blurOfFNumber(float f)
        {
            f = llclamp(f, F_WIDE, F_NARROW);
            return 100.f * (logf(F_NARROW) - logf(f)) / (logf(F_NARROW) - logf(F_WIDE));
        }

        // A number box that is only shown (the focus distance): the slider's value box without the slider. It is
        // laid out across the room after its label; the box is drawn at the right, VALUE_WIDTH wide for a number
        // and as wide as a word needs (up to the room, then cut with "...") for a word such as "click a place".
        class ValueBox : public LLView
        {
        public:
            explicit ValueBox(const LLView::Params& p) : LLView(p), mFont(ui::font(st::FONT_BODY)) {}

            void setText(const std::string& text)
            {
                if (text != mText)
                {
                    mText  = text;
                    mTextW = utf8str_to_wstring(text);
                    mBoxW  = llmax(F32(st::VALUE_WIDTH), std::ceil(mFont->getWidthF32(mTextW.c_str())) + 2.f * st::VALUE_PAD);
                }
            }

            void draw() override
            {
                const F32 w = F32(getRect().getWidth());
                const F32 h = F32(getRect().getHeight());
                const F32 a = getDrawContext().mAlpha * (getEnabled() ? 1.f : 0.4f);
                const F32 box = llmin(mBoxW, w);
                ui::drawRound(LLRectf(w - box, h, w, 0.f), st::VALUE_RADIUS, ui::Fill::solid(ui::faded(st::VALUE_FILL, a)));
                ui::drawText(mFont, mTextW, w - F32(st::VALUE_PAD), std::floor(h * 0.5f), ui::faded(st::TEXT_WHITE, a),
                             LLFontGL::RIGHT, LLFontGL::VCENTER, llmax(0, S32(box) - 2 * st::VALUE_PAD), true);
            }

        private:
            const LLFontGL* mFont;
            std::string mText;
            LLWString   mTextW;
            F32         mBoxW = F32(st::VALUE_WIDTH);
        };

        // The focus distance row's value when there is no distance to show (the distance itself is >= 0).
        constexpr float NO_DISTANCE     = -1.f;     // "-": the official focus (auto, the cursor), a target not seen
        constexpr float CLICK_NONE      = -2.f;     // "the place clicked" without a place, not picking: the button
        constexpr float CLICK_WAITING   = -3.f;     // ... picking: a click on the world
        constexpr float DOF_OFF         = -4.f;     // "-", the depth of field is off: its tip says so

        // The words and the tip of the focus distance row for a value that is not a distance.
        const char* distanceWord(float v)
        {
            return v == CLICK_WAITING ? "focus_click_waiting" : v == CLICK_NONE ? "focus_click_none" : "no_distance";
        }
        const char* distanceTip(float v)
        {
            return v == CLICK_WAITING ? "focus_click_waiting_tip"
                 : v == CLICK_NONE    ? "focus_click_none_tip"
                 : v == DOF_OFF       ? "focus_distance_dof_tip"
                                      : "focus_distance_tip";
        }

        // The depth of field is on: the aperture, the bokeh and the focus distance mean something.
        bool dofOn()
        {
            static LLCachedControl<bool> dof(gSavedSettings, items::ITEM_DEPTH_OF_FIELD, false);
            return dof;
        }

        // A line of the sections as it is laid out: up to three widgets side by side.
        enum class LineKind { Header, Sub, Slider, Combo, Value, Switch, Divider, Buttons };
        struct Line
        {
            LineKind kind;
            int      section;
            LLView*  a = nullptr;   // the header, label, slider, divider or first button
            LLView*  b = nullptr;   // the switch, drop-down, value box or second button
            LLView*  c = nullptr;   // the icon button after a drop-down
        };

        // A widget whose value follows the viewer's.
        enum class RowKind { Slider, Combo, Toggle, Distance };
        struct Row
        {
            RowKind kind;
            int     section;
            LLView* view  = nullptr;
            LLView* label = nullptr;            // greyed out with the widget
            std::function<float()> get;
            std::function<bool()>  usable;      // null: always
            pm::FeatureGate feature;            // and the feature it needs (catalog "needs_feature")
            bool  shown_set = false;
            float shown     = 0.f;
            int   enabled   = -1;
        };

        constexpr size_t NO_ROW = static_cast<size_t>(-1);

        struct Right
        {
            LLHandle<LLPanel>        built;     // the panel the rows were made for
            std::vector<Line>        lines;
            std::vector<Row>         rows;
            std::vector<std::string> lookIds;   // the drop-down's rows: PlusLook values
            std::string              look;      // PlusLook as the last tick read it
            std::string              lastLook;  // the last look that was on (the switch turns it on again)
            std::vector<fl::Nearby>  nearby, nearbyShown;   // "対象": listed every tick, shown the same as left's did
            bool peopleListed = false;
            ui::PsCombo*  focusWho  = nullptr;  // "対象": raw pointers, its options are rebuilt outside the row system
            LLScrollListItem* focusNobody = nullptr;   // its "Nobody" row (not choosable; remade with the options)
            size_t focusWhoRow = NO_ROW;        // its row in sR.rows (its value is shown by refreshFocusWho())
            ui::PsButton* focusPick = nullptr;  // "クリックで指定": its label follows the picking state
            int pickShown   = -1;                // the label shown: 1 picking, 0 not, -1 not written yet
            ui::PsLabel* saveDirLabel = nullptr;    // "Save to: <folder>" of the output section; remade with the panel
            std::string saveDirShown;           // its text as written (with the 'default' note), to write on a change only
            int pickEnabled = -1;                // the button's enabled state shown, likewise
            const char* distanceTipShown = nullptr;   // the focus distance's tip shown (a key of distanceTip()), null: not yet
            S32 mode = 0;                       // the shutter: 0 a picture, 1 the 360° capture
            S32 contentWidth = 0;
        };
        Right sR;

        bool sectionShown(int section)
        {
            return section == SecColor || sS.sectionOpen[section];  // color grading has a switch, not a fold
        }

        std::string text(const char* name)
        {
            LLPanel* panel = sS.right.get();
            return panel ? panel->getString(name) : std::string();
        }

        template <class T>
        T* make(typename T::Params p, LLView* parent, const std::string& name, S32 width, S32 height)
        {
            p.name(name);
            p.rect(LLRect(0, height, width, 0));
            T* widget = LLUICtrlFactory::create<T>(p);
            if (widget)
            {
                parent->addChild(widget);
            }
            return widget;
        }

        // --- making the rows ----------------------------------------------------------------------------

        struct Builder
        {
            LLView* content;
            S32     width;

            void header(int section, const char* key, const char* icon, bool foldable, bool line_above,
                        std::function<float()> on_get = nullptr, std::function<void(bool)> on_set = nullptr,
                        const char* tip = nullptr)
            {
                ui::PsHeader::Params p(LLUICtrlFactory::getDefaultParams<ui::PsHeader>());
                p.label(text(key));
                p.size("right");
                p.divider(line_above ? "top" : "none");
                p.collapsible(foldable);
                if (icon)
                {
                    p.icon(icon);
                }
                ui::PsHeader* head = make<ui::PsHeader>(p, content, std::string("h_") + SEC_IDS[section], width, HEADER_H);
                if (!head)
                {
                    return;
                }
                head->setValue(LLSD(sectionShown(section)));
                if (foldable)
                {
                    head->setCommitCallback([section](LLUICtrl* ctrl, const LLSD&) {
                        sS.sectionOpen[section] = ctrl->getValue().asBoolean();
                        fillRight();    // lays the rows out again (nothing is made or deleted)
                        tickRight();    // the rows that came into view, at once
                    });
                }
                Line line { LineKind::Header, section, head };
                if (on_get)
                {
                    line.b = toggle(section, std::string("sw_") + SEC_IDS[section], on_get, on_set, nullptr, tip);
                }
                sR.lines.push_back(line);
            }

            void sub(int section, const char* key, std::function<float()> on_get = nullptr,
                     std::function<void(bool)> on_set = nullptr, const char* tip = nullptr)
            {
                ui::PsHeader::Params p(LLUICtrlFactory::getDefaultParams<ui::PsHeader>());
                p.label(text(key));
                p.size("sub");
                p.collapsible(false);
                if (tip)
                {
                    p.tool_tip(text(tip));
                }
                Line line { LineKind::Sub, section, make<ui::PsHeader>(p, content, std::string("sh_") + key, width, SUB_H) };
                if (on_get)
                {
                    line.b = toggle(section, std::string("sw_") + key, on_get, on_set, nullptr, tip);
                }
                sR.lines.push_back(line);
            }

            // A switch whose value follows on_get (a row of its own for the tick).
            ui::PsSwitch* toggle(int section, const std::string& name, std::function<float()> on_get,
                                 std::function<void(bool)> on_set, LLView* label, const char* tip,
                                 std::function<bool()> usable = nullptr)
            {
                ui::PsSwitch::Params p(LLUICtrlFactory::getDefaultParams<ui::PsSwitch>());
                if (tip)
                {
                    p.tool_tip(text(tip));
                }
                ui::PsSwitch* sw = make<ui::PsSwitch>(p, content, name, SWITCH_W, ROW_H);
                if (!sw)
                {
                    return nullptr;
                }
                sw->setCommitCallback([on_set](LLUICtrl* ctrl, const LLSD&) { on_set(ctrl->getValue().asBoolean()); });
                sR.rows.push_back({ RowKind::Toggle, section, sw, label, std::move(on_get), std::move(usable) });
                return sw;
            }

            void slider(int section, const std::string& label, const std::string& tip, F32 lo, F32 hi, F32 step,
                        S32 digits, const char* unit, bool sign, std::function<float()> on_get,
                        std::function<void(float)> on_set, std::function<bool()> usable = nullptr,
                        std::optional<float> default_value = std::nullopt)
            {
                ui::PsSlider::Params p(LLUICtrlFactory::getDefaultParams<ui::PsSlider>());
                p.label(label);
                if (!tip.empty())
                {
                    p.tool_tip(tip);
                }
                p.min_value(lo);
                p.max_value(hi);
                p.increment(step);
                p.decimal_digits(digits);
                if (unit)
                {
                    p.unit(unit);
                }
                p.show_sign(sign);
                if (default_value)
                {
                    p.default_value(*default_value);
                }
                ui::PsSlider* s = make<ui::PsSlider>(p, content, "s_" + label, width - 2 * MARGIN, ROW_H);
                if (!s)
                {
                    return;
                }
                s->setCommitCallback([on_set](LLUICtrl* ctrl, const LLSD&) { on_set((float)ctrl->getValue().asReal()); });
                sR.lines.push_back({ LineKind::Slider, section, s });
                sR.rows.push_back({ RowKind::Slider, section, s, nullptr, std::move(on_get), std::move(usable) });
            }

            ui::PsLabel* label(const std::string& words, const std::string& tip)
            {
                ui::PsLabel::Params p(LLUICtrlFactory::getDefaultParams<ui::PsLabel>());
                p.label(words);
                if (!tip.empty())
                {
                    p.tool_tip(tip);
                    p.mouse_opaque(true);   // for its tool tip
                }
                return make<ui::PsLabel>(p, content, "l_" + words, LABEL_W, ROW_H);
            }

            // A drop-down; options: the rows' words and values. extra: an icon button after it (or null).
            ui::PsCombo* combo(int section, const std::string& words, const std::string& tip,
                               const std::vector<std::pair<std::string, S32>>& options, std::function<float()> on_get,
                               std::function<void(S32)> on_set, std::function<bool()> usable = nullptr,
                               const std::vector<bool>* enabled = nullptr)
            {
                ui::PsLabel* name = label(words, tip);
                ui::PsCombo::Params p(LLUICtrlFactory::getDefaultParams<ui::PsCombo>());
                if (!tip.empty())
                {
                    p.tool_tip(tip);
                }
                ui::PsCombo* box = make<ui::PsCombo>(p, content, "c_" + words, 200, COMBO_H);
                if (!box)
                {
                    return nullptr;
                }
                for (size_t i = 0; i < options.size(); ++i)
                {
                    box->add(options[i].first, LLSD(options[i].second), ADD_BOTTOM, !enabled || (*enabled)[i]);
                }
                box->setCommitCallback([on_set](LLUICtrl* ctrl, const LLSD&) { on_set(ctrl->getValue().asInteger()); });
                sR.lines.push_back({ LineKind::Combo, section, name, box });
                sR.rows.push_back({ RowKind::Combo, section, box, name, std::move(on_get), std::move(usable) });
                return box;
            }

            void switchRow(int section, const std::string& words, const std::string& tip, std::function<float()> on_get,
                           std::function<void(bool)> on_set, std::function<bool()> usable = nullptr)
            {
                ui::PsLabel* name = label(words, tip);
                ui::PsSwitch::Params p(LLUICtrlFactory::getDefaultParams<ui::PsSwitch>());
                if (!tip.empty())
                {
                    p.tool_tip(tip);
                }
                ui::PsSwitch* sw = make<ui::PsSwitch>(p, content, "sw_" + words, SWITCH_W, ROW_H);
                if (!sw)
                {
                    return;
                }
                sw->setCommitCallback([on_set](LLUICtrl* ctrl, const LLSD&) { on_set(ctrl->getValue().asBoolean()); });
                sR.lines.push_back({ LineKind::Switch, section, name, sw });
                sR.rows.push_back({ RowKind::Toggle, section, sw, name, std::move(on_get), std::move(usable) });
            }

            // A photo tools' item as a row of its kind; words: the row's name (empty: the item's).
            void item(int section, const char* id, const char* key, const char* unit = nullptr,
                      std::function<bool()> usable = nullptr)
            {
                const pm::Item* found = pm::findItem(id);
                if (!found || found->kind == pm::Kind::Text)
                {
                    return;
                }
                const pm::Item* it = found;    // items() lives as long as the process
                const std::string words = key ? text(key) : it->label;
                if (!usable && !it->needs.empty())
                {
                    if (LLControlVariable* needs = gSavedSettings.getControl(it->needs))
                    {
                        const int value = it->needs_value;
                        usable = [needs, value]() { return pm::needsMet(needs, value); };
                    }
                }
                std::function<float()> get = [it]() { return pm::itemValue(*it); };
                const size_t first = sR.rows.size();
                switch (it->kind)
                {
                    case pm::Kind::Check:
                        switchRow(section, words, it->note, get, [it](bool on) { pm::setItemValue(*it, on ? 1.f : 0.f); },
                                  std::move(usable));
                        break;
                    case pm::Kind::Combo:
                    {
                        std::vector<std::pair<std::string, S32>> options;
                        std::vector<bool> selectable;
                        for (const pm::Option& option : it->options)
                        {
                            options.emplace_back(option.label, (S32)ll_round(option.value));
                            selectable.push_back(!option.heading);
                        }
                        combo(section, words, it->note, options, get, [it](S32 v) { pm::setItemValue(*it, (float)v); },
                              std::move(usable), &selectable);
                        break;
                    }
                    default:
                    {
                        const float scale = it->scale;
                        std::optional<float> def = pm::itemDefaultValue(*it);
                        if (def)
                        {
                            def = *def * scale;
                        }
                        slider(section, words, it->note, it->min * scale, it->max * scale, it->step * scale, it->digits,
                               unit, false, [it, scale]() { return pm::itemValue(*it) * scale; },
                               [it, scale](float v) { pm::setItemValue(*it, v / scale); }, std::move(usable), def);
                        break;
                    }
                }
                if (sR.rows.size() > first)
                {
                    // the widget and its name were made with the note as their tip
                    pm::FeatureGate& feature = sR.rows.back().feature;
                    feature.feature = it->feature;
                    feature.tip     = it->note;
                }
            }

            void distance(int section)
            {
                ui::PsLabel* name = label(text("focus_distance"), text("focus_distance_tip"));
                LLView::Params p;
                p.name("v_focus_distance");
                p.rect(LLRect(0, st::VALUE_HEIGHT, st::VALUE_WIDTH, 0));
                p.mouse_opaque(true);
                p.tool_tip(text("focus_distance_tip"));
                ValueBox* box = new ValueBox(p);
                content->addChild(box);
                sR.lines.push_back({ LineKind::Value, section, name, box });
                sR.rows.push_back({ RowKind::Distance, section, box, name, []() {
                    if (!dofOn())
                    {
                        return DOF_OFF;     // no focus at all (fl::focusDistance() says false): not "click"
                    }
                    F32 meters = 0.f;
                    if (fl::focusDistance(meters))
                    {
                        return std::round(meters * 10.f) / 10.f;
                    }
                    if (fl::getMode() == fl::MODE_CLICK && !fl::hasClickPoint())
                    {
                        return fl::isPicking() ? CLICK_WAITING : CLICK_NONE;    // what to do, not "-"
                    }
                    return NO_DISTANCE;
                } });
            }

            void divider(int section)
            {
                ui::PsDivider::Params p(LLUICtrlFactory::getDefaultParams<ui::PsDivider>());
                p.kind("right");
                sR.lines.push_back({ LineKind::Divider, section,
                                     make<ui::PsDivider>(p, content, "div_" + std::to_string(sR.lines.size()), width, DIVIDER_H) });
            }

            ui::PsButton* button(const char* key, const char* tip, std::function<void()> action)
            {
                ui::PsButton::Params p(LLUICtrlFactory::getDefaultParams<ui::PsButton>());
                p.label(text(key));
                p.tool_tip(text(tip));
                ui::PsButton* b = make<ui::PsButton>(p, content, std::string("b_") + key, 160, BUTTON_H);
                if (b)
                {
                    b->setCommitCallback([action](LLUICtrl*, const LLSD&) { action(); });
                }
                return b;
            }
        };

        // A look is on: the colour grading's rows can be used.
        bool lookOn()
        {
            return !sR.look.empty();
        }

        // The format is JPEG (SnapshotFormat 1): its quality can be used.
        bool jpegChosen()
        {
            static LLCachedControl<S32> format(gSavedSettings, "SnapshotFormat", 0);
            return format == 1;
        }

        std::function<bool()> usableFor(items::Gate gate)
        {
            switch (gate)
            {
                case items::Gate::WithLook: return &lookOn;
                case items::Gate::WithJpeg: return &jpegChosen;
                default:                    return nullptr;     // the catalog's "needs" only
            }
        }

        // The rows of a section that places the photo tools' items by hand (photoscreen_items.h), in the table's
        // order; an id the catalog does not have is left out (Builder::item).
        void addPlaced(Builder& b, int section, items::Spot spot)
        {
            items::forEachRow(spot, [&b, section](const items::Placement& placement) {
                b.item(section, placement.id, placement.widget, nullptr, usableFor(placement.gate));
            });
        }

        // "対象" (research/264): the combo's value is an index into sR.nearbyShown (0: nobody), not the UUID
        // itself, so it fits the row system's plain S32 values (as "部位" does with fl::Part already).
        float focusWhoIndex()
        {
            const LLUUID target = fl::getAvatar();
            if (target.isNull())
            {
                return 0.f;
            }
            for (size_t i = 0; i < sR.nearbyShown.size(); ++i)
            {
                if (sR.nearbyShown[i].id == target)
                {
                    return float(i + 1);
                }
            }
            return 0.f;
        }

        void setFocusWhoIndex(S32 v)
        {
            const size_t i = (size_t)(v - 1);
            if (v >= 1 && i < sR.nearbyShown.size())
            {
                fl::setAvatar(sR.nearbyShown[i].id);
            }
        }

        // Shows the option `index` (0: "Nobody"). The row system's setComboIfChanged() cannot: the combo box
        // selects only enabled rows (LLScrollListCtrl::setSelectedByValue(), LLComboBox::setCurrentByIndex()),
        // and "Nobody" is not one, so it would leave the box blank. It is enabled just for the selecting.
        void showFocusWho(S32 index)
        {
            ui::PsCombo* who = sR.focusWho;
            if (index == 0 && sR.focusNobody)
            {
                sR.focusNobody->setEnabled(true);
                who->setCurrentByIndex(0);
                sR.focusNobody->setEnabled(false);
            }
            else
            {
                who->setCurrentByIndex(index);
            }
            if (sR.focusWhoRow < sR.rows.size())
            {
                Row& row = sR.rows[sR.focusWhoRow];     // so that tickRight()'s loop does not set it again
                row.shown_set = true;
                row.shown     = (float)index;
            }
        }

        // The people near refresh while the row is shown (tickRight()), as the left column's focus tab did
        // (photodetail_left.cpp, before it moved here): rebuilt only when who is near actually changed; the
        // option shown is set again after a rebuild (the names resolved, someone came or left) and whenever
        // the target changed.
        void refreshFocusWho()
        {
            if (!sR.focusWho)
            {
                return;
            }
            fl::listNearby(sR.nearby);
            bool same = sR.peopleListed && sR.nearby.size() == sR.nearbyShown.size();
            for (size_t i = 0; same && i < sR.nearby.size(); ++i)
            {
                same = sR.nearby[i].id == sR.nearbyShown[i].id && sR.nearby[i].name == sR.nearbyShown[i].name;
            }
            if (!same)
            {
                sR.peopleListed = true;
                sR.nearbyShown  = sR.nearby;
                sR.focusWho->removeall();
                sR.focusNobody = sR.focusWho->add(str("Nobody"), LLSD(0), ADD_BOTTOM, false);
                for (size_t i = 0; i < sR.nearbyShown.size(); ++i)
                {
                    sR.focusWho->add(sR.nearbyShown[i].name, LLSD((S32)(i + 1)));
                }
            }
            const S32 index = (S32)focusWhoIndex();
            const bool written = sR.focusWhoRow < sR.rows.size() && sR.rows[sR.focusWhoRow].shown_set &&
                                 sR.rows[sR.focusWhoRow].shown == (float)index;
            if (!same || !written)
            {
                showFocusWho(index);
            }
        }

        // The f-number, the focal length and the angle of view are three views of two settings.
        void addCamera(Builder& b)
        {
            const std::function<bool()> dof_on = &dofOn;

            b.header(SecCamera, "sec_camera", "camera", true, false);
            if (const pm::Item* angle = pm::findItem(items::ITEM_CAMERA_ANGLE))
            {
                const std::optional<float> angle_def = pm::itemDefaultValue(*angle);
                const float deg = angle->scale;     // radians to degrees
                b.slider(SecCamera, text("focal"), text("focal_tip"), std::floor(angleToMm(angle->max)),
                         std::ceil(angleToMm(angle->min)), 1.f, 0, " mm", false,
                         [angle]() { return angleToMm(pm::itemValue(*angle)); },
                         [angle](float mm) { pm::setItemValue(*angle, llclamp(mmToAngle(mm), angle->min, angle->max)); },
                         nullptr, angle_def ? std::optional<float>(angleToMm(*angle_def)) : std::nullopt);
                b.slider(SecCamera, text("fov"), text("fov_tip"), angle->min * deg, angle->max * deg, 0.5f, 1, "°", false,
                         [angle, deg]() { return pm::itemValue(*angle) * deg; },
                         [angle, deg](float v) { pm::setItemValue(*angle, llclamp(v / deg, angle->min, angle->max)); },
                         nullptr, angle_def ? std::optional<float>(*angle_def * deg) : std::nullopt);
            }

            // The depth of field's switch heads the rows that need it (the bokeh just under it, then the aperture and the
            // focus), so a row greyed out with it off has its reason right above it.
            b.divider(SecCamera);
            if (const pm::Item* dof_item = pm::findItem(items::ITEM_DEPTH_OF_FIELD))
            {
                // Rebuilds the pipeline: the viewer stands still for seconds (as in the photo tools).
                b.sub(SecCamera, "sub_dof", [dof_item]() { return pm::itemValue(*dof_item); },
                      [dof_item](bool on) { pm::setItemValue(*dof_item, on ? 1.f : 0.f); }, "dof_tip");
            }
            if (LLControlVariable* cof = gSavedSettings.getControl(SETTING_MAX_COF))
            {
                b.slider(SecCamera, text("bokeh"), text("bokeh_tip"), 0.f, MAX_COF_LIMIT, 0.5f, 1, nullptr, false,
                         [cof]() { return llclamp((float)cof->getValue().asReal(), 0.f, MAX_COF_LIMIT); },
                         [cof](float v) { cof->setValue(LLSD((LLSD::Real)llclamp(v, 0.f, MAX_COF_LIMIT))); }, dof_on,
                         llclamp((float)cof->getDefault().asReal(), 0.f, MAX_COF_LIMIT));
            }
            if (LLControlVariable* fnumber = gSavedSettings.getControl(SETTING_FNUMBER))
            {
                b.slider(SecCamera, text("aperture"), text("aperture_tip"), F_WIDE, F_NARROW, 0.1f, 1, nullptr, false,
                         [fnumber]() { return llclamp((float)fnumber->getValue().asReal(), F_WIDE, F_NARROW); },
                         [](float f) { pm::setValue(pm::Param::Blur, blurOfFNumber(f)); }, dof_on,
                         // "D": f/22, the photo tools' background blur 0 (pm::paramDefault(Blur), no blur), not
                         // CameraFNumber's own default (9.0, a plain camera's): both rows reset to the same.
                         F_NARROW);
            }

            const std::vector<std::pair<std::string, S32>> modes = {
                { text("focus_auto"), fl::MODE_OFF }, { text("focus_cursor"), fl::MODE_CURSOR },
                { text("focus_self"), fl::MODE_SELF }, { text("focus_other"), fl::MODE_OTHER },
                { text("focus_click"), fl::MODE_CLICK } };
            b.combo(SecCamera, text("focus_mode"), text("focus_mode_tip"), modes, []() { return (float)fl::getMode(); },
                    [](S32 v) {
                        const auto mode = static_cast<fl::Mode>(llclamp(v, (S32)fl::MODE_OFF, (S32)fl::MODE_COUNT - 1));
                        if (mode == fl::MODE_OTHER && fl::getAvatar().isNull() && fl::listNearby(sR.nearby) > 0)
                        {
                            fl::setAvatar(sR.nearby.front().id);    // the nearest, until someone is chosen (as the left)
                        }
                        fl::setMode(mode);
                        if (mode == fl::MODE_OTHER)
                        {
                            refreshFocusWho();  // the list is kept up only in this mode: bring it up to date now
                        }
                    });
            {
                static LLCachedControl<bool> frame(gSavedSettings, SETTING_FRAME, true);
                ui::PsIconButton::Params p(LLUICtrlFactory::getDefaultParams<ui::PsIconButton>());
                p.icon("scan_face");
                p.toggle(true);
                p.tool_tip(text("focus_frame_tip"));
                if (ui::PsIconButton* frame_button = make<ui::PsIconButton>(p, b.content, "ib_focus_frame", ICON_BTN_W, COMBO_H))
                {
                    frame_button->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                        gSavedSettings.setBOOL(SETTING_FRAME, ctrl->getValue().asBoolean());
                    });
                    if (!sR.lines.empty() && sR.lines.back().kind == LineKind::Combo)
                    {
                        sR.lines.back().c = frame_button;
                    }
                    sR.rows.push_back({ RowKind::Toggle, SecCamera, frame_button, nullptr, []() { return frame ? 1.f : 0.f; } });
                }
            }
            // The left column's "ピント" section moved here (research/264): who and which part the "相手の顔"
            // mode follows, and the button that starts picking a place for the "クリックした場所" mode. Always
            // shown, greyed out like the rest of the block when the chosen mode does not use them.
            sR.focusWho = b.combo(SecCamera, text("focus_who"), text("focus_who_tip"), {}, &focusWhoIndex,
                                  &setFocusWhoIndex, []() { return fl::getMode() == fl::MODE_OTHER; });
            sR.focusWhoRow = sR.focusWho ? sR.rows.size() - 1 : NO_ROW;
            const std::vector<std::pair<std::string, S32>> parts = { { str("PartEyes"), fl::PART_EYES },
                                                                      { str("PartChest"), fl::PART_CHEST },
                                                                      { str("PartBody"), fl::PART_BODY } };
            b.combo(SecCamera, text("focus_part"), std::string(), parts, []() { return (float)fl::getPart(); },
                    [](S32 v) { fl::setPart(static_cast<fl::Part>(llclamp(v, (S32)fl::PART_EYES, (S32)fl::PART_BODY))); },
                    []() { return fl::getMode() == fl::MODE_SELF || fl::getMode() == fl::MODE_OTHER; });
            sR.focusPick = b.button("focus_click_start", "focus_click_tip", []() { fl::setPicking(!fl::isPicking()); });
            if (sR.focusPick)
            {
                sR.lines.push_back({ LineKind::Buttons, SecCamera, sR.focusPick });    // one button across the line
            }
            b.distance(SecCamera);
        }

        // Shown as in the target picture: 0 in the middle, - to +. Exposure in stops. 0 is each one's own
        // "D" default (photomode::paramDefault(): Brightness, Contrast and Shadow are all their formula's
        // fixed point, so the scaled -1..1 reading is 0 too).
        void addExposure(Builder& b)
        {
            b.header(SecExposure, "sec_exposure", "sun", true, true);
            b.slider(SecExposure, text("exposure"), text("exposure_tip"), -1.f, 1.f, 0.1f, 1, nullptr, true,
                     []() { return pm::value(pm::Param::Brightness) / 100.f; },
                     [](float v) { pm::setValue(pm::Param::Brightness, v * 100.f); }, nullptr, 0.f);
            b.slider(SecExposure, text("contrast"), text("contrast_tip"), -1.f, 1.f, 0.1f, 1, nullptr, true,
                     []() { return (pm::value(pm::Param::Contrast) - 50.f) / 50.f; },
                     [](float v) { pm::setValue(pm::Param::Contrast, 50.f + v * 50.f); }, nullptr, 0.f);
            // + lifts the shadows (as a photo editor's "Shadows"): less of the photo tools' shadow depth.
            b.slider(SecExposure, text("shadows"), text("shadows_tip"), -1.f, 1.f, 0.1f, 1, nullptr, true,
                     []() { return (50.f - pm::value(pm::Param::Shadow)) / 50.f; },
                     [](float v) { pm::setValue(pm::Param::Shadow, 50.f - v * 50.f); }, nullptr, 0.f);
            // Local contrast (research/249): the look pass's, the photo tools' items (their tips).
            addPlaced(b, SecExposure, items::Spot::RightExposure);
        }

        void addColor(Builder& b)
        {
            b.header(SecColor, "sec_color", nullptr, false, true, []() { return lookOn() ? 1.f : 0.f; },
                     [](bool on) {
                         if (!on)
                         {
                             if (!sR.look.empty())
                             {
                                 sR.lastLook = sR.look;
                             }
                             pm::applyLook(std::string());
                         }
                         else if (!sR.lastLook.empty() || !sR.lookIds.empty())
                         {
                             pm::applyLook(sR.lastLook.empty() ? sR.lookIds.front() : sR.lastLook);
                         }
                         sR.look = pm::currentLook();
                     },
                     "grading_tip");

            std::vector<std::pair<std::string, S32>> looks;
            sR.lookIds.clear();
            for (const pm::LookChoice& look : pm::allLooks())
            {
                if (look.exists && !look.id.empty())
                {
                    looks.emplace_back(look.label.empty() ? look.id : look.label, (S32)sR.lookIds.size());
                    sR.lookIds.push_back(look.id);
                }
            }
            b.combo(SecColor, text("lut"), text("lut_tip"), looks,
                    []() {
                        const std::string& shown = sR.look.empty() ? sR.lastLook : sR.look;
                        for (size_t i = 0; i < sR.lookIds.size(); ++i)
                        {
                            if (sR.lookIds[i] == shown)
                            {
                                return (float)i;
                            }
                        }
                        return 0.f;
                    },
                    [](S32 i) {
                        if (i >= 0 && (size_t)i < sR.lookIds.size())
                        {
                            pm::applyLook(sR.lookIds[i]);
                            sR.look = sR.lookIds[i];
                        }
                    },
                    &lookOn);
            addPlaced(b, SecColor, items::Spot::RightColor);
        }

        void addEffects(Builder& b)
        {
            b.header(SecEffects, "sec_effects", "effects", true, true);
            addPlaced(b, SecEffects, items::Spot::RightEffects);
        }

        void addOutput(Builder& b)
        {
            // Size and aspect ratio are the chips above the world (photodetail_center.cpp, "chip_res" /
            // "chip_aspect"); this block does not repeat them.
            b.header(SecOutput, "sec_output", "output", true, true);
            addPlaced(b, SecOutput, items::Spot::RightOutput);
            ui::PsButton* last = b.button("last_shot", "last_shot_tip", []() { pm::openLastShot(); });
            sR.lines.push_back({ LineKind::Buttons, SecOutput, last });
            // The folder is the snapshot window's (SnapshotBaseDir): shown here, changed here, no setting of our own.
            // The line's text and tip are written by tickSaveDir().
            sR.saveDirLabel = b.label("-", "-");
            sR.saveDirShown.clear();
            sR.lines.push_back({ LineKind::Sub, SecOutput, sR.saveDirLabel });
            ui::PsButton* choose = b.button("change_folder", "change_folder_tip", []() { pm::chooseSaveDir(); });
            ui::PsButton* folder = b.button("open_folder", "open_folder_tip", []() {
                if (!pm::openSaveDir())
                {
                    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text("save_dir_missing")));
                }
            });
            sR.lines.push_back({ LineKind::Buttons, SecOutput, choose, folder });
        }

        // The shutter strip of the XML: what its widgets do.
        void bindShutter(LLPanel* panel)
        {
            ui::PsShutter* shutter = panel->findChild<ui::PsShutter>("shutter_button");
            if (ui::PsSegment* segment = panel->findChild<ui::PsSegment>("seg_mode"))
            {
                segment->setValue(LLSD(sR.mode));
                segment->setCommitCallback([shutter](LLUICtrl* ctrl, const LLSD&) {
                    sR.mode = ctrl->getValue().asInteger();
                    if (shutter)
                    {
                        shutter->setToolTip(text(sR.mode ? "shutter_360_tip" : "shutter_photo_tip"));
                    }
                });
            }
            if (shutter)
            {
                shutter->setToolTip(text(sR.mode ? "shutter_360_tip" : "shutter_photo_tip"));
                shutter->setCommitCallback([](LLUICtrl*, const LLSD&) {
                    if (sR.mode)
                    {
                        LLFloaterReg::showInstance("360capture");
                    }
                    else
                    {
                        pm::takePhoto();
                    }
                });
            }
            if (ui::PsIconButton* tools = panel->findChild<ui::PsIconButton>("ib_settings"))
            {
                tools->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance(TOOLS_FLOATER); });
            }
        }

        void build(LLPanel* panel)
        {
            sR.lines.clear();     // the widgets of an earlier panel went with it
            sR.saveDirLabel = nullptr;
            sR.rows.clear();
            sR.built = panel->getHandle();
            sR.contentWidth = 0;
            sR.look = pm::currentLook();
            sR.focusWho = nullptr;
            sR.focusNobody = nullptr;
            sR.focusWhoRow = NO_ROW;
            sR.focusPick = nullptr;
            sR.peopleListed = false;
            sR.nearbyShown.clear();
            sR.pickShown   = -1;
            sR.pickEnabled = -1;
            sR.distanceTipShown = nullptr;
            LLPanel* content = panel->findChild<LLPanel>("content");
            if (!content)
            {
                return;
            }
            Builder b { content, content->getRect().getWidth() };
            addCamera(b);
            addExposure(b);
            addColor(b);
            addEffects(b);
            addOutput(b);
            bindShutter(panel);
        }

        // --- laying the rows out ------------------------------------------------------------------------

        void place(LLView* view, S32 left, S32 top, S32 right, S32 bottom)
        {
            if (view)
            {
                view->setShape(LLRect(left, top, llmax(left, right), llmin(top, bottom)));
                view->setVisible(true);
            }
        }

        S32 lineHeight(LineKind kind)
        {
            switch (kind)
            {
                case LineKind::Header:  return HEADER_H;
                case LineKind::Sub:     return SUB_H;
                case LineKind::Combo:   return COMBO_ROW_H;
                case LineKind::Divider: return DIVIDER_H;
                case LineKind::Buttons: return BUTTON_ROW_H;
                default:                return ROW_H;
            }
        }

        // Walks the lines from the top: their tops (y down from the content's top) to `at`, and the height.
        template <class F>
        S32 walk(F&& at)
        {
            S32 y = 0;
            bool rows_since_header = false;
            for (const Line& line : sR.lines)
            {
                const bool header = line.kind == LineKind::Header;
                if (!header && !sectionShown(line.section))
                {
                    at(line, -1);
                    continue;
                }
                if (header)
                {
                    if (rows_since_header)
                    {
                        y += SECTION_END;
                    }
                    rows_since_header = false;
                }
                at(line, y);
                y += lineHeight(line.kind);
                if (header && sectionShown(line.section))
                {
                    y += AFTER_HEADER;
                }
                rows_since_header = rows_since_header || !header;
            }
            return rows_since_header ? y + SECTION_END : y;
        }

        void layoutContent()
        {
            LLPanel* panel = sS.right.get();
            LLScrollContainer* scroll = panel ? panel->findChild<LLScrollContainer>("scroll") : nullptr;
            LLPanel* content = panel ? panel->findChild<LLPanel>("content") : nullptr;
            if (!scroll || !content || sR.lines.empty())
            {
                return;
            }
            const S32 w = llmax(0, scroll->getRect().getWidth() - SCROLLBAR);
            const S32 h = walk([](const Line&, S32) {});
            if (content->getRect().getWidth() != w || content->getRect().getHeight() != h)
            {
                content->reshape(w, h);
            }
            sR.contentWidth = w;
            const S32 x0 = MARGIN;
            const S32 x1 = w - MARGIN;
            walk([w, h, x0, x1](const Line& line, S32 y) {
                if (y < 0)
                {
                    for (LLView* view : { line.a, line.b, line.c })
                    {
                        if (view)
                        {
                            view->setVisible(false);
                        }
                    }
                    return;
                }
                const S32 top = h - y;
                const S32 bottom = top - lineHeight(line.kind);
                switch (line.kind)
                {
                    case LineKind::Header:
                        place(line.a, 0, top, w, bottom);
                        place(line.b, x1 - SWITCH_W, top, x1, bottom);
                        break;
                    case LineKind::Sub:
                        place(line.a, x0, top, line.b ? x1 - SWITCH_W - SIDE_GAP : x1, bottom);
                        place(line.b, x1 - SWITCH_W, top, x1, bottom);
                        break;
                    case LineKind::Combo:
                    {
                        const S32 ctop = top - (COMBO_ROW_H - COMBO_H) / 2;
                        place(line.a, x0, top, x0 + LABEL_W, bottom);
                        place(line.b, x0 + LABEL_W + st::SLIDER_GAP, ctop, line.c ? x1 - ICON_BTN_W - 4 : x1, ctop - COMBO_H);
                        place(line.c, x1 - ICON_BTN_W, ctop, x1, ctop - COMBO_H);
                        break;
                    }
                    case LineKind::Value:
                    {
                        const S32 vtop = top - (ROW_H - st::VALUE_HEIGHT) / 2;
                        place(line.a, x0, top, x0 + LABEL_W, bottom);
                        // the room after the label: the box draws itself at the right of it (ValueBox)
                        place(line.b, llmin(x0 + LABEL_W + st::SLIDER_GAP, x1 - st::VALUE_WIDTH), vtop, x1, vtop - st::VALUE_HEIGHT);
                        break;
                    }
                    case LineKind::Switch:
                        place(line.a, x0, top, x1 - SWITCH_W - SIDE_GAP, bottom);
                        place(line.b, x1 - SWITCH_W, top, x1, bottom);
                        break;
                    case LineKind::Divider:
                        place(line.a, 0, top, w, bottom);
                        break;
                    case LineKind::Buttons:
                    {
                        const S32 mid = (x0 + x1) / 2;
                        place(line.a, x0, top, line.b ? mid - BUTTON_GAP / 2 : x1, top - BUTTON_H);
                        place(line.b, mid + BUTTON_GAP / 2, top, x1, top - BUTTON_H);
                        break;
                    }
                    default:    // a slider
                        place(line.a, x0, top, x1, bottom);
                        break;
                }
            });
        }
    }

    // --- the right panel -----------------------------------------------------------------------------------

    void fillRight()
    {
        LLPanel* right = sS.right.get();
        if (!right)
        {
            return;
        }
        if (sR.built.get() != right)
        {
            build(right);
        }
        layoutContent();
    }

    void layoutRight(const Margins& m)
    {
        LLPanel* right = sS.right.get();
        if (!right)
        {
            return;
        }
        right->setVisible(m.wide);
        if (!m.wide)
        {
            return;
        }
        // The whole height at the right, as in the target picture; in front of the strip under the world.
        right->setShape(LLRect(sS.w - m.right_panel, sS.h, sS.w, 0));
        if (LLView* parent = right->getParent())
        {
            parent->sendChildToFront(right);
        }
        const S32 w = m.right_panel;
        const S32 h = sS.h;
        const S32 shutter_top = GAP + SHUTTER_H;
        if (LLView* shutter = right->findChildView("shutter", false))
        {
            shutter->setShape(LLRect(GAP, shutter_top, llmax(GAP, w - GAP), GAP));
            if (LLView* button = shutter->findChildView("shutter_button", false))
            {
                const LLRect r = button->getRect();
                button->setOrigin((shutter->getRect().getWidth() - r.getWidth()) / 2, r.mBottom);
            }
        }
        if (LLView* sections = right->findChildView("sections", false))
        {
            const S32 bottom = shutter_top + GAP;
            sections->setShape(LLRect(GAP, llmax(bottom, h - GAP), llmax(GAP, w - GAP), bottom));
            if (LLView* scroll = sections->findChildView("scroll", false))
            {
                const LLRect box = sections->getLocalRect();
                scroll->setShape(LLRect(0, llmax(0, box.getHeight() - SCROLL_INSET), box.getWidth(),
                                        llmin(SCROLL_INSET, box.getHeight())));
            }
        }
        fillRight();
    }

    void openRightSection(int section)
    {
        LLPanel* right = sS.right.get();
        if (!right || section < 0 || section >= SecCount)
        {
            return;
        }
        sS.sectionOpen[section] = true;
        fillRight();
        LLView* head = right->findChildView(std::string("h_") + SEC_IDS[section], true);
        if (LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(head))
        {
            ctrl->setValue(LLSD(true));    // setValue() does not commit, so the header's callback does not run
        }
        tickRight();
        LLScrollContainer* scroll = right->findChild<LLScrollContainer>("scroll");
        if (head && scroll && head->getParent())
        {
            scroll->scrollToShowRect(head->getRect());
        }
    }

    namespace
    {
        // The output section's folder line, written when the folder changed (also by the snapshot window).
        void tickSaveDir()
        {
            LLPanel* panel = sS.right.get();
            if (!panel)
            {
                return;
            }
            const pm::SaveDirView view = pm::saveDirView();
            LLStringUtil::format_map_t args;
            args["[DIR]"] = view.dir;
            const std::string shown = panel->getString(view.key, args);
            if (shown != sR.saveDirShown)
            {
                sR.saveDirShown = shown;
                sR.saveDirLabel->setLabelText(shown);
                sR.saveDirLabel->setToolTip(shown);   // the whole line, as the label may cut it
            }
        }
    }

    void tickRight()
    {
        LLPanel* right = sS.right.get();
        if (!right || !right->getVisible() || sR.built.get() != right)
        {
            return;
        }
        sR.look = pm::currentLook();
        if (!sR.look.empty() && sR.look != sR.lastLook)
        {
            sR.lastLook = sR.look;
        }
        if (sectionShown(SecCamera))
        {
            // The people near are listed only while the "相手の顔" mode uses them (the row is greyed out
            // otherwise), and once at first so that the greyed box is not blank; the mode's own setter lists
            // them at once when it changes to "相手の顔".
            if (fl::getMode() == fl::MODE_OTHER || !sR.peopleListed)
            {
                refreshFocusWho();
            }
            if (ui::PsButton* pick = sR.focusPick)
            {
                const bool usable = fl::getMode() == fl::MODE_CLICK;
                if (sR.pickEnabled != (int)usable)
                {
                    sR.pickEnabled = (int)usable;
                    pick->setEnabled(usable);
                }
                const bool picking = fl::isPicking();
                if (sR.pickShown != (int)picking)
                {
                    sR.pickShown = (int)picking;
                    pick->setLabelText(text(picking ? "focus_click_stop" : "focus_click_start"));
                }
            }
        }
        if (sR.saveDirLabel && sectionShown(SecOutput))
        {
            tickSaveDir();
        }
        LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
        for (Row& row : sR.rows)
        {
            if (!row.view || !sectionShown(row.section))
            {
                continue;
            }
            const bool usable = row.feature.usable(row.view, row.label) && (!row.usable || row.usable());
            if (row.enabled != (usable ? 1 : 0))
            {
                row.enabled = usable ? 1 : 0;
                row.view->setEnabled(usable);
                if (row.label)
                {
                    row.label->setEnabled(usable);
                }
            }
            const float v = row.get();
            if (row.shown_set && v == row.shown)
            {
                continue;
            }
            switch (row.kind)
            {
                case RowKind::Slider:
                    // not while it is dragged or typed into: its own value is the newer
                    if (gFocusMgr.childHasMouseCapture(row.view) || (focus && focus != row.view && focus->hasAncestor(row.view)))
                    {
                        continue;
                    }
                    static_cast<LLUICtrl*>(row.view)->setValue(LLSD((LLSD::Real)(v > pm::UNSET ? v : 0.f)));
                    break;
                case RowKind::Combo:
                    setComboIfChanged(static_cast<LLComboBox*>(row.view), LLSD((LLSD::Integer)ll_round(v)));
                    break;
                case RowKind::Toggle:
                    static_cast<LLUICtrl*>(row.view)->setValue(LLSD(v > 0.5f));
                    break;
                case RowKind::Distance:
                {
                    static_cast<ValueBox*>(row.view)->setText(v < 0.f ? text(distanceWord(v)) : llformat("%.1f m", v));
                    // the tip says why there is no distance, and the whole of a word cut short in a narrow panel;
                    // written only when it changes, not with each 0.1 m
                    const char* tip = distanceTip(v);
                    if (tip != sR.distanceTipShown)
                    {
                        sR.distanceTipShown = tip;
                        row.view->setToolTip(text(tip));
                    }
                    break;
                }
            }
            row.shown_set = true;
            row.shown     = v;
        }
    }
}
