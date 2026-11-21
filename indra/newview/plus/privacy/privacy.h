/**
 * @file privacy.h
 * @brief Keeps the changed settings and the names of the Charis windows out of what
 *        the viewer sends to Linden Lab (the ViewerStats report).
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
// its statistics (send_viewer_stats(), llviewerstats.cpp: body["preferences"]) to
// the region's ViewerStats capability, and writes the same body to the log (INFO,
// "LogViewerStatsPacket"). Charis does not send them (2026-11-21): what the user
// changed is not for Linden Lab, and a key the user typed in (the translation keys)
// or a Charis setting must not leave by this way (research/123 H2). The rest of the
// statistics (the viewer's name and version, the frame rate, the hardware ...) is
// sent as the official viewer does.
//
// Hook: one call in send_viewer_stats() (llviewerstats.cpp), after the body is made
// and before it is logged or sent.

namespace plus::privacy
{
    // Removes body["preferences"] (the changed settings), and from the UI counts
    // (body["ui"]: commands, controls, floaters, panels) the names of the Charis
    // windows, panels and commands ("plus_", "Plus_", "panel_plus_", "fs_poser",
    // "Poser_" ...; research/130 L3).
    void filterStats(LLSD& body);
}

#endif // PLUS_PRIVACY_H
