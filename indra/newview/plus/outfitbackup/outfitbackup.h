/**
 * @file outfitbackup.h
 * @brief A local copy of the agent's outfit and attachment placements,
 *        offered back at login when the server has an older one.
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

#ifndef PLUS_OUTFITBACKUP_H
#define PLUS_OUTFITBACKUP_H

// The feature registers itself (plus::FeatureRegistration) and runs from an
// idle callback; nothing in the official viewer calls it.
//
// Setting: PlusOutfitBackup (off by default). Whether the copy is kept is decided at
//          login; turned off later, the recording stops at once (read every frame).
// File:    <account folder>/plus_outfit_backup.xml on the main grid (Agni),
//          <account folder>/plus_outfit_backup.<grid>.xml on others (e.g. .aditi)
// Log tag: OutfitBackup

#endif // PLUS_OUTFITBACKUP_H
