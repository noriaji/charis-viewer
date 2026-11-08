/**
 * @file settingstools.h
 * @brief MCP tools of the viewer's local endpoint that read and change the
 *        viewer's settings (the photo tools' settings and a short list of
 *        render settings): settings_list, settings_get, settings_set,
 *        settings_revert. For testing and A/B measurements by an assistant.
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

#ifndef PLUS_POSERMCP_SETTINGSTOOLS_H
#define PLUS_POSERMCP_SETTINGSTOOLS_H

#include "posertools.h"

#include <string>
#include <vector>

namespace plus::posermcp
{
    // settings_list, settings_get, settings_set, settings_revert. Every handler
    // parses its arguments and does all viewer work through run(), on the main thread.
    void appendSettingsTools(std::vector<plus::mcp::Tool>& tools, const MainThreadRunner& run);

    // A paragraph for the server's instructions.
    std::string settingsInstructions();

    // Main thread, after the server stopped (its jobs are done or failed). With
    // restore_values, the settings that settings_set changed without persist go
    // back to the values from before (each one only while it still holds the
    // value the tool gave it: a change made since by the user stays). The
    // session's change history is forgotten either way. At quit the values are
    // left as they are (restore_values false): such values are never written to
    // the user's settings file anyway (they are the control's unsaved value).
    void settingsServerStopped(bool restore_values);

    // Main thread. True while value is a test value settings_set wrote (persist false) and still
    // owns: the stop would take it back. Others that remember "the user's value" (the photo tools'
    // baseline and compare) take the saved value instead of such a value (fix-round100).
    bool isTestValue(const std::string& name, const LLSD& value);
}

#endif // PLUS_POSERMCP_SETTINGSTOOLS_H
