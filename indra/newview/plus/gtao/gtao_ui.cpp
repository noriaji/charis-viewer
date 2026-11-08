/**
 * @file gtao_ui.cpp
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

// Charis's own. GTAO and SSGI need more than their own settings (the
// backend, which safe mode turns off, the official SSAO, HDR, the reflection
// probes: plus::gtao::unavailableReason(), plus::ssgi::unavailableReason()).
// Without them the check box did nothing that could be seen; now the rows are
// greyed out and their tool tips say why. The rows of the photo tools and the
// photo screen do the same through the catalog's "needs_feature"
// (plus/photomode/phototools.h featureUnavailable()).
// While Advanced Graphics shows, the reasons and the two switches are looked
// at every WATCH_SECONDS, so a change made elsewhere (the photo tools, the
// photo screen, the MCP tools, HDR or the probes in another window) greys the
// rows at once (research/275 L066, L074). Only pointers and two flags are
// compared; nothing runs while the floater is hidden.

#include "llviewerprecompiledheaders.h"

#include "gtao_ui.h"

#include "gtao.h"

#include "ssgi/ssgi.h"

#include "plus/photoscreen/psui/psstyle.h"

#include "llapp.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "llpanel.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

#include <map>

namespace
{
    // The preferences' rows (floater_preferences_graphics_advanced.xml, the
    // Charis tab): each feature's check box, then the rows used only while it
    // is on.
    constexpr const char* GTAO_SWITCH = "PlusGtao";
    constexpr const char* GTAO_ROWS[] = { "PlusGtaoQualityText", "PlusGtaoQuality", "PlusGtaoStrength", "PlusGtaoRadius" };
    constexpr const char* SSGI_SWITCH = "PlusSsgi";
    constexpr const char* SSGI_ROWS[] = { "PlusSsgiStrength" };
    constexpr F32 WATCH_SECONDS = 0.25f;

    // A row's tool tip as its XUI has it, kept the first time it is seen (before
    // a reason is put in front of it). The floater is made again from the same
    // XUI, so the tips stay the same for the session.
    const std::string& ownTip(const LLUICtrl& ctrl)
    {
        static std::map<std::string, std::string> tips;
        auto found = tips.find(ctrl.getName());
        if (found == tips.end())
        {
            found = tips.emplace(ctrl.getName(), ctrl.getToolTip()).first;
        }
        return found->second;
    }

    // usable: the row can be used; reason: why the feature cannot (nullptr: it can).
    void showRow(LLPanel& floater, const char* name, bool usable, const char* reason)
    {
        LLUICtrl* ctrl = floater.findChild<LLUICtrl>(name);
        if (!ctrl)
        {
            return;
        }
        const std::string tip = plus::gtao::tipWithReason(reason, ownTip(*ctrl));
        ctrl->setEnabled(usable);
        if (ctrl->getToolTip() != tip)
        {
            ctrl->setToolTip(tip);
        }
    }

    // A feature's check box (usable when the feature can be), then its rows
    // (usable when it can be and is on: its setting).
    template <size_t N>
    void showFeature(LLPanel& floater, const char* switch_name, const char* const (&rows)[N], const char* reason)
    {
        showRow(floater, switch_name, !reason, reason);
        const LLControlVariable* setting = gSavedSettings.getControl(switch_name);
        const bool on = !reason && setting && setting->getValue().asBoolean();
        for (const char* row : rows)
        {
            showRow(floater, row, on, reason);
        }
    }

    // What the rows show depends on: the reasons (pointers to fixed strings) and the switches.
    struct RowState
    {
        const char* gtao_reason = nullptr;
        const char* ssgi_reason = nullptr;
        bool        gtao_on = false;
        bool        ssgi_on = false;

        bool operator==(const RowState&) const = default;
    };

    RowState rowState()
    {
        static LLCachedControl<bool> gtao_on(gSavedSettings, GTAO_SWITCH, false);
        static LLCachedControl<bool> ssgi_on(gSavedSettings, SSGI_SWITCH, false);
        RowState state;
        state.gtao_reason = plus::gtao::unavailableReason();
        state.ssgi_reason = plus::ssgi::unavailableReason();
        state.gtao_on = gtao_on;
        state.ssgi_on = ssgi_on;
        return state;
    }

    LLHandle<LLPanel> sWatched;     // the floater whose rows were shown last
    RowState          sShown;       // what they show
    bool              sWatching = false;

    // Started by each refresh; ends when the floater is hidden or gone (it starts again when it opens).
    void watch(LLPanel& floater)
    {
        sWatched = floater.getHandle();
        if (sWatching)
        {
            return;
        }
        sWatching = true;
        doPeriodically([]()
            {
                LLPanel* shown = sWatched.get();
                if (LLApp::isExiting() || !shown || !shown->isInVisibleChain())
                {
                    sWatching = false;
                    return true;
                }
                if (rowState() != sShown)
                {
                    plus::gtao::refreshPreferenceRows(*shown);
                }
                return false;
            }, WATCH_SECONDS);
    }
}

namespace plus::gtao
{
    std::string reasonText(const char* reason)
    {
        if (!reason)
        {
            return std::string();
        }
        static constinit plus::psui::StringsPanel strings("panel_plus_gtao_reasons.xml");
        return strings.has(reason) ? strings.get(reason) : std::string(reason);
    }

    std::string tipWithReason(const char* reason, const std::string& tip)
    {
        if (!reason)
        {
            return tip;
        }
        const std::string why = reasonText(reason);
        return tip.empty() ? why : why + "\n\n" + tip;
    }

    void refreshPreferenceRows(LLPanel& floater)
    {
        sShown = rowState();
        showFeature(floater, GTAO_SWITCH, GTAO_ROWS, sShown.gtao_reason);
        showFeature(floater, SSGI_SWITCH, SSGI_ROWS, sShown.ssgi_reason);
        watch(floater);
    }
}
