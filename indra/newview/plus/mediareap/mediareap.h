/**
 * @file mediareap.h
 * @brief Media plugins nobody uses are shut down early (plus/mediareap)
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

#ifndef PLUS_MEDIAREAP_H
#define PLUS_MEDIAREAP_H

#include "llpluginclassmedia.h"

class LLViewerMediaImpl;

// Once a second (an idle callback) the plugins of media nobody uses are shut down: in-world media
// not drawn and out of earshot, UI browsers whose floater is closed (details in mediareap.cpp).
// PlusMediaReap (on by default) switches it.
namespace plus::mediareap
{
    // LLViewerMedia::updateMedia(), for each impl, after the official priority is chosen and before
    // it is counted: an impl whose plugin this shut down stays PRIORITY_UNLOADED while the official
    // priority is PRIORITY_HIDDEN or lower, so a navigate (a script changing the media URL) does not
    // start the plugin again; drawn, shown or focused again, it gets the official priority and loads.
    // Main thread, every frame: a set lookup (nothing when no plugin was shut down).
    LLPluginClassMedia::EPriority priorityFor(LLViewerMediaImpl* impl, LLPluginClassMedia::EPriority priority);
}

#endif // PLUS_MEDIAREAP_H
