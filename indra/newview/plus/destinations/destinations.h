/**
 * @file destinations.h
 * @brief Reload for the Destinations floater (plus/destinations)
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

#ifndef PLUS_DESTINATIONS_H
#define PLUS_DESTINATIONS_H

class LLFloater;

namespace plus
{
namespace destinations
{
    // postBuild: wires the floater's "plus_reload_btn" to reload the page on screen
    // without the cache (the guide URL only when no page is loaded).
    void setupReloadButton(LLFloater* floater);

    // onOpen: reloads the guide if its last load ended in an HTTP error. A page that
    // loaded fine is left as it is (opening the floater does not read the page again).
    void reloadIfLastLoadFailed(LLFloater* floater);
}
}

#endif // PLUS_DESTINATIONS_H
