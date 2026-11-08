/**
 * @file gtao_ui.h
 * @brief Why GTAO and SSGI cannot be used, on the rows that turn them on:
 *        the texts of plus::gtao::reason and the graphics preferences' rows.
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

#ifndef PLUS_GTAO_UI_H
#define PLUS_GTAO_UI_H

#include <string>

class LLPanel;

namespace plus::gtao
{
    // The text of a reason (plus::gtao::unavailableReason(),
    // plus::ssgi::unavailableReason()) in the viewer's language
    // (panel_plus_gtao_reasons.xml, en and ja); empty for nullptr. After the
    // UI is there. Main thread.
    std::string reasonText(const char* reason);

    // A row's tool tip while it cannot be used: why, then its own tip; tip
    // alone for nullptr.
    std::string tipWithReason(const char* reason, const std::string& tip);

    // The <plus> hook at the end of
    // LLFloaterPreferenceGraphicsAdvanced::refreshEnabledState() (called when
    // the floater opens and when a graphics setting is committed there): GTAO's
    // and SSGI's rows of the Charis tab greyed out while they cannot be used,
    // the reason first in their tool tips. While the floater shows, it follows
    // changes made elsewhere too (a check every 0.25 s; research/275 L066, L074).
    void refreshPreferenceRows(LLPanel& floater);
}

#endif // PLUS_GTAO_UI_H
