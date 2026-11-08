/**
 * @file safemode_badge.h
 * @brief Safe mode (research/254): the small "Safe mode" box at the bottom
 *        right of the screen while a safe start runs, and the texts of the
 *        feature's UI.
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

#ifndef PLUS_SAFEMODE_BADGE_H
#define PLUS_SAFEMODE_BADGE_H

#include "llstring.h"

#include <string>

namespace plus::safemode
{
    // A text of panel_plus_safemode.xml (en, ja: the box, the alerts), with args put in; the
    // name when there is none. After the UI is there. Main thread.
    std::string uiText(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t());
}

namespace plus::safemode::badge
{
    // Once, when the UI is there (the login screen), in a safe start only: a normal start
    // makes no view at all. Main thread.
    void attach();
}

#endif // PLUS_SAFEMODE_BADGE_H
