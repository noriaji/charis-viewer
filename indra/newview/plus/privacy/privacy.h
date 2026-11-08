/**
 * @file privacy.h
 * @brief Keeps secrets and Charis' own settings out of what the viewer sends
 *        to Linden Lab (the ViewerStats report at logout and quit).
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

#ifndef PLUS_PRIVACY_H
#define PLUS_PRIVACY_H

class LLSD;

// The official viewer sends the settings that differ from their defaults with
// its statistics (send_viewer_stats(), llviewerstats.cpp: gSavedSettings and
// gSavedPerAccountSettings asLLSD(true), which leaves out only settings of the
// String type) to the region's ViewerStats capability, and writes the same
// body to the log (INFO, "LogViewerStatsPacket"). The translation keys
// AzureTranslateAPIKey and DeepLTranslateAPIKey are of the LLSD type, so a key
// the user typed in went to Linden Lab and into SecondLife.log
// (research/123 H2).
//
// Hook: one call in send_viewer_stats() (llviewerstats.cpp), after the
// preferences are put in the body and before it is logged or sent.

namespace plus::privacy
{
    // Removes from body["preferences"]["settings"] and
    // body["preferences"]["settings_per_account"]:
    //   - every setting that is not Boolean, S32, U32, F32 or a colour (and
    //     any the control group does not know). LLSD settings hold anything,
    //     such as UserLoginInfo, where "--login first last password" puts the
    //     password as typed (research/130 H2), login URIs and device GUIDs;
    //     Vector3D ones where you were (CameraPosOnLogout, FocusPosOnLogout);
    //     String ones (asLLSD() already skips them) what you typed. Only
    //     Boolean, number and colour settings are left;
    //   - UserLoginInfo by name as well, and every setting whose name
    //     contains (any case) "key", "token", "secret", "password", "passwd"
    //     or "api";
    //   - every setting of the Charis features (every name settings_plus.xml
    //     declares, RenderScreenSpaceReflection* and MiniMapShowChatRange
    //     among them, and any name starting with "Plus" or "FS"): camera
    //     positions, pins and other records of what the user did. Also the
    //     settings Charis added to the official settings.xml (RenderOITMode,
    //     RenderOITSnapshotFull; the list in privacy.cpp, which
    //     tools/predeliver.py checks against the official release), and
    //     RenderTonemapType when it holds a Charis tone map (2 and up).
    // Setting PlusSendSettingsStats (default on; Preferences > Privacy,
    // panel_plus_privacy_stats.xml) off: removes
    // body["preferences"] altogether (the rest of the statistics, the same
    // as every viewer sends, stays).
    // Also removes from the UI counts (body["ui"]: commands, controls,
    // floaters, panels) the names of the Charis windows, panels and
    // commands ("plus_", "Plus_", "panel_plus_", "fs_poser", "Poser_" ...;
    // research/130 L3), with or without preferences in the body.
    void filterStats(LLSD& body);
}

#endif // PLUS_PRIVACY_H
