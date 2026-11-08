/**
 * @file aotransfer.h
 * @brief Animation Overrider: sets to and from files (".charis-ao.xml"),
 *        and Firestorm / ZHAO notecards into sets.
 *
 * The file is LLSD XML:
 *   format  "charis-ao", version 1, exported (date)
 *   sets    [ { name, sit_override, smart, mouselook_stand_disable,
 *               states { "<state name>": { cycle, random, cycle_time,
 *                        animations [ { name, asset_id, item_id } ] } } } ]
 * item_id is the animation item the AO's link points to; asset_id is written
 * only for a full permission animation (copy, modify and transfer). On import every
 * animation is looked up in the agent's inventory by asset id, then item
 * id, then name; animations the agent does not have are listed, not made.
 *
 * Notecard import is Firestorm's (AOEngine::importNotecard()/parseNotecard()):
 * lines "[ State ]Anim1|Anim2", the animations in the notecard's folder.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Zi Ree @ Second Life
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

#ifndef PLUS_AO_AOTRANSFER_H
#define PLUS_AO_AOTRANSFER_H

#include <string>
#include <vector>

class LLViewerInventoryItem;

namespace plus::ao
{
    class AOSet;

    // The file name ending for exports (picked as XML files).
    extern const char EXPORT_FILE_SUFFIX[]; // ".charis-ao.xml"

    // A file name for the export of sets named name (characters a file name
    // cannot have replaced).
    std::string exportFileName(const std::string& name);

    // Writes the sets to path. False if it could not be written.
    bool exportSets(const std::vector<const AOSet*>& sets, const std::string& path);

    // Reads path and creates its sets; reports the result (aofloater.cpp's strings).
    void importFile(const std::string& path);

    // Downloads the notecard and creates a set of its name from it; reports
    // the result when done. False if it cannot be started.
    bool importNotecard(const LLViewerInventoryItem* item);
}

#endif // PLUS_AO_AOTRANSFER_H
