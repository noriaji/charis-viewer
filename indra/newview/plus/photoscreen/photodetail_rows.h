/**
 * @file photodetail_rows.h
 * @brief The rows of the side panels of the detailed photo screen (the photo tools' items as sliders,
 *        checks and choices), used by the left and the right panel.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_ROWS_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_ROWS_H

#include "photodetail_state.h"

class LLFlatListView;

namespace plus::photoscreen::detail
{
    // One row of a panel: what it shows and writes (the photo tools' item, or a function of ours).
    struct Spec
    {
        std::string label, tip;
        pm::Kind    kind  = pm::Kind::Slider;
        float       min = 0.f, max = 100.f, step = 1.f;
        int         digits = 0;
        float       scale  = 1.f;       // shown value = value * scale
        std::vector<pm::Option> options;
        std::function<float()>     get;
        std::function<void(float)> set;
        LLControlVariable* needs = nullptr; // the row works only while this setting is on (catalog "needs")
        int  needs_value = -1;
        bool blur = false;                  // needs the depth of field (pm::blurAvailable())
        pm::Feature feature = pm::Feature::None;    // needs that feature (catalog "needs_feature")
    };

    void clearRows(LLFlatListView* list, std::vector<RowRef>& rows);
    bool addRow(LLFlatListView* list, std::vector<RowRef>& rows, const Spec& spec);
    void addButtonRow(LLFlatListView* list, const std::string& label, const std::string& tip, std::function<void()> action);
    Spec specOf(const pm::Item& item);
    bool addLookRow(LLFlatListView* list, std::vector<RowRef>& rows, const pm::Item& item);
    bool addItemRow(LLFlatListView* list, std::vector<RowRef>& rows, const pm::Item& item);
    bool addItemRowById(LLFlatListView* list, std::vector<RowRef>& rows, const char* id);
    void addTabItems(LLFlatListView* list, std::vector<RowRef>& rows, const char* tab, const char* skip = nullptr);
    void addSettingChoice(LLFlatListView* list, std::vector<RowRef>& rows, const char* label, const char* setting,
                          const char* prefix, int count);

    // The focal length of a 35 mm lens for the vertical angle of view, and back.
    float angleToMm(float angle);
    float mmToAngle(float mm);
    void addFocalRow(LLFlatListView* list, std::vector<RowRef>& rows);

    // The widgets of the rows from the values now (each written only when its value changed).
    void refreshRows(std::vector<RowRef>& rows);
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_ROWS_H
