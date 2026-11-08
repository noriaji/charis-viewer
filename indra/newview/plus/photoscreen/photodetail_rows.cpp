/**
 * @file photodetail_rows.cpp
 * @brief The rows of the side panels of the detailed photo screen (moved out of photodetail.cpp).
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

#include "photodetail_rows.h"

#include "plus/photomode/photolook.h"

#include <cmath>

#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llflatlistview.h"
#include "llfocusmgr.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

namespace plus::photoscreen::detail
{
    // --- rows ------------------------------------------------------------------

    void clearRows(LLFlatListView* list, std::vector<RowRef>& rows)
    {
        rows.clear();   // the widgets are deleted with the list's items
        list->clear();
    }

    // A row from a spec; false when its XML did not load.
    bool addRow(LLFlatListView* list, std::vector<RowRef>& rows, const Spec& spec)
    {
        const char* file = spec.kind == pm::Kind::Check ? "panel_plus_photoscreen_row_check.xml"
                         : spec.kind == pm::Kind::Combo ? "panel_plus_photoscreen_row_combo.xml"
                                                        : "panel_plus_photoscreen_row_slider.xml";
        LLPanel* row = loadPanel(file);
        if (!row)
        {
            return false;
        }
        LLUICtrl* ctl = row->getChild<LLUICtrl>("ctl");
        RowRef ref;
        ref.ctl = ctl;
        ref.kind = spec.kind;
        ref.scale = spec.scale;
        ref.options = spec.options;
        ref.get = spec.get;
        ref.needs = spec.needs;
        ref.needs_value = spec.needs_value;
        ref.blur = spec.blur;
        ref.feature.feature = spec.feature;
        ref.feature.tip = spec.tip;

        if (spec.kind == pm::Kind::Check)
        {
            static_cast<LLCheckBoxCtrl*>(ctl)->setLabel(spec.label);
        }
        else if (LLTextBox* label = row->findChild<LLTextBox>("label"))
        {
            label->setText(spec.label);
            label->setToolTip(spec.tip.empty() ? spec.label : spec.label + "\n" + spec.tip);
        }
        if (!spec.tip.empty())
        {
            ctl->setToolTip(spec.tip);
        }

        const std::function<void(float)> set = spec.set;
        const float scale = spec.scale;
        if (spec.kind == pm::Kind::Slider)
        {
            LLSliderCtrl* slider = static_cast<LLSliderCtrl*>(ctl);
            slider->setMinValue(spec.min * scale);
            slider->setMaxValue(spec.max * scale);
            slider->setIncrement(spec.step * scale);
            slider->setPrecision(spec.digits);
            ctl->setCommitCallback([set, scale](LLUICtrl* c, const LLSD&) { set((float)c->getValue().asReal() / scale); });
        }
        else if (spec.kind == pm::Kind::Check)
        {
            ctl->setCommitCallback([set](LLUICtrl* c, const LLSD&) { set(c->getValue().asBoolean() ? 1.f : 0.f); });
        }
        else
        {
            LLComboBox* combo = static_cast<LLComboBox*>(ctl);
            for (const pm::Option& option : spec.options)
            {
                combo->add(option.label, LLSD((LLSD::Integer)ll_round(option.value)), ADD_BOTTOM, !option.heading);
            }
            ctl->setCommitCallback([set](LLUICtrl* c, const LLSD&) { set((float)c->getValue().asReal()); });
        }
        list->addItem(row, LLSD(spec.label));
        rows.push_back(std::move(ref));
        return true;
    }

    void addButtonRow(LLFlatListView* list, const std::string& label, const std::string& tip, std::function<void()> action)
    {
        LLPanel* row = loadPanel("panel_plus_photoscreen_row_button.xml");
        if (!row)
        {
            return;
        }
        LLButton* button = row->getChild<LLButton>("ctl");
        button->setLabel(label);
        button->setToolTip(tip.empty() ? label : tip);
        button->setCommitCallback([action](LLUICtrl*, const LLSD&) { action(); });
        list->addItem(row, LLSD(label));
    }

    // The photo tools' item as a spec.
    Spec specOf(const pm::Item& item)
    {
        Spec spec;
        const pm::Item* p = &item;      // items() lives as long as the process
        spec.label = item.label;
        spec.tip   = item.note;
        spec.kind  = item.kind;
        spec.min = item.min;
        spec.max = item.max;
        spec.step = item.step;
        spec.digits = item.digits;
        spec.scale = item.scale;
        spec.options = item.options;
        spec.get = [p]() { return pm::itemValue(*p); };
        spec.set = [p](float v) { pm::setItemValue(*p, v); };
        if (!item.needs.empty())
        {
            spec.needs = gSavedSettings.getControl(item.needs);
            spec.needs_value = item.needs_value;
        }
        spec.blur = item.param == pm::Param::Blur;
        spec.feature = item.feature;
        return spec;
    }

    // The look list as a choice: the value is the index in lookIds.
    bool addLookRow(LLFlatListView* list, std::vector<RowRef>& rows, const pm::Item& item)
    {
        Spec spec;
        spec.label = item.label;
        spec.tip   = item.note;
        spec.kind  = pm::Kind::Combo;
        sS.lookIds.clear();
        float index = 0.f;
        for (const pm::LookChoice& look : pm::allLooks())
        {
            if (!look.exists)
            {
                continue;
            }
            spec.options.push_back({ index, look.label.empty() ? str("LookNone") : look.label });
            sS.lookIds.push_back(look.id);
            index += 1.f;
        }
        spec.get = []() {
            const std::string now = pm::currentLook();
            for (size_t i = 0; i < sS.lookIds.size(); ++i)
            {
                if (sS.lookIds[i] == now)
                {
                    return (float)i;
                }
            }
            return 0.f;
        };
        spec.set = [](float v) {
            const size_t i = (size_t)llmax(0, ll_round(v));
            if (i < sS.lookIds.size())
            {
                pm::applyLook(sS.lookIds[i]);
            }
        };
        return addRow(list, rows, spec);
    }

    bool addItemRow(LLFlatListView* list, std::vector<RowRef>& rows, const pm::Item& item)
    {
        if (item.kind == pm::Kind::Text)
        {
            return false;
        }
        if (item.composite == "look")
        {
            return addLookRow(list, rows, item);
        }
        return addRow(list, rows, specOf(item));
    }

    bool addItemRowById(LLFlatListView* list, std::vector<RowRef>& rows, const char* id)
    {
        const pm::Item* item = pm::findItem(id);
        return item && addItemRow(list, rows, *item);
    }

    void addTabItems(LLFlatListView* list, std::vector<RowRef>& rows, const char* tab, const char* skip)
    {
        for (const pm::Item& item : pm::items())
        {
            if (item.tab == tab && !item.detail && (!skip || item.id != skip))
            {
                addItemRow(list, rows, item);
            }
        }
    }

    // A setting of the viewer as a row of choices (labels from the strings PhotoScreenDetail_<prefix><n>).
    void addSettingChoice(LLFlatListView* list, std::vector<RowRef>& rows, const char* label, const char* setting,
                          const char* prefix, int count)
    {
        LLControlVariable* control = gSavedSettings.getControl(setting);
        if (!control)
        {
            return;
        }
        Spec spec;
        spec.label = str(label);
        spec.kind  = pm::Kind::Combo;
        for (int i = 0; i < count; ++i)
        {
            spec.options.push_back({ (float)i, str(std::string(prefix) + std::to_string(i)) });
        }
        spec.get = [control]() { return (float)control->getValue().asInteger(); };
        spec.set = [control](float v) { control->setValue(LLSD((LLSD::Integer)ll_round(v))); };
        addRow(list, rows, spec);
    }

    // --- the focal length, the read-out ----------------------------------------------

    // The vertical angle of view of a 35 mm lens (24 mm high film): the focal length that gives CameraAngle.
    float angleToMm(float angle)
    {
        return 12.f / tanf(llclamp(angle, 0.05f, 3.f) * 0.5f);
    }

    float mmToAngle(float mm)
    {
        return 2.f * atanf(12.f / llmax(mm, 1.f));
    }

    void addFocalRow(LLFlatListView* list, std::vector<RowRef>& rows)
    {
        const pm::Item* angle = pm::findItem("CameraAngle");
        if (!angle)
        {
            return;
        }
        Spec spec;
        spec.label  = str("FocalLength");
        spec.tip    = str("FocalLengthTip");
        spec.min    = std::floor(angleToMm(angle->max));
        spec.max    = std::ceil(angleToMm(angle->min));
        spec.step   = 1.f;
        spec.get    = [angle]() { return angleToMm(pm::itemValue(*angle)); };
        spec.set    = [angle](float mm) { pm::setItemValue(*angle, llclamp(mmToAngle(mm), angle->min, angle->max)); };
        addRow(list, rows, spec);
    }

    void refreshRows(std::vector<RowRef>& rows)
    {
        for (RowRef& row : rows)
        {
            if (!row.ctl)
            {
                continue;
            }
            const float v = row.get();
            if (!row.shown_set || v != row.shown)
            {
                row.shown_set = true;
                row.shown     = v;
                if (row.kind == pm::Kind::Slider)
                {
                    // not while it is dragged: its own value is the newer
                    if (!gFocusMgr.childHasMouseCapture(row.ctl))
                    {
                        static_cast<LLSliderCtrl*>(row.ctl)->setValue((v > pm::UNSET ? v : 0.f) * row.scale, false);
                    }
                    else
                    {
                        row.shown_set = false;
                    }
                }
                else if (row.kind == pm::Kind::Check)
                {
                    row.ctl->setValue(LLSD(v > 0.5f));
                }
                else
                {
                    const pm::Option* nearest = nullptr;
                    for (const pm::Option& option : row.options)
                    {
                        if (!option.heading && (!nearest || fabsf(option.value - v) < fabsf(nearest->value - v)))
                        {
                            nearest = &option;
                        }
                    }
                    if (nearest)
                    {
                        setComboIfChanged(static_cast<LLComboBox*>(row.ctl), LLSD((LLSD::Integer)ll_round(nearest->value)));
                    }
                }
            }
            bool usable = pm::needsMet(row.needs, row.needs_value);
            if (row.blur)
            {
                usable = usable && pm::blurAvailable();
            }
            usable = row.feature.usable(row.ctl) && usable;
            if (row.enabled != (usable ? 1 : 0))
            {
                row.enabled = usable ? 1 : 0;
                row.ctl->setEnabled(usable);
            }
        }
    }
}
