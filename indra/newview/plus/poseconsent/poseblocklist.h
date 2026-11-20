/**
 * @file poseblocklist.h
 * @brief The people whose pose requests are dropped ("block future requests" of the confirmation), kept per
 *        account in <account folder>/plus_pose_consent.xml (research/336 4.1).
 *
 * At most MAX_ENTRIES: past that no one more is taken (as Aurora; no one is taken off to make room), and the caller tells the person. The file is read the first time it is needed
 * after a login (a bad or foreign one is put aside, and the list starts empty), and written whole through a
 * temporary file (plus::writeFileReplacing) at every change. Main thread only.
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

#ifndef PLUS_POSEBLOCKLIST_H
#define PLUS_POSEBLOCKLIST_H

#include <cstddef>

class LLUUID;

namespace plus::poseconsent::blocklist
{
    constexpr std::size_t MAX_ENTRIES = 256;
    constexpr char FILE_NAME[] = "plus_pose_consent.xml";   // also in plus/importsettings' list of files not brought over

    enum class Added
    {
        Added,          // on the list now (and saved)
        AlreadyThere,
        Full,           // MAX_ENTRIES already: not added
        Unavailable     // not logged in, or not someone that can be listed (null, ourselves)
    };

    bool contains(const LLUUID& agent_id);
    Added add(const LLUUID& agent_id);
    std::size_t count();
    // Empties the list (and the file).
    void clear();
    // Drops the list held (called at a logout while the ledger holds something). Not needed for a change of account:
    // the list is read again whenever the account logged in is not the one it was read for.
    void forget();
}

#endif // PLUS_POSEBLOCKLIST_H
