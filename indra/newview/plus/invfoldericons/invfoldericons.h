/**
 * @file invfoldericons.h
 * @brief Inventory folder icons: one colour and one emblem per system folder
 *        type (Favorites, Outfits, Animations, Objects...), so they stand out
 *        from each other and from plain folders, as in Firestorm.
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

#ifndef PLUS_INVFOLDERICONS_H
#define PLUS_INVFOLDERICONS_H

#include "llfoldertype.h"
#include "lluuid.h"

#include <string>

// The icons are Charis' own (made by make_icons.py in this folder, no image of
// another viewer; see ORIGIN.md). Firestorm does the same with one texture
// name per folder type in llviewerfoldertype.cpp; here the official table
// stays as it is and one hook picks our name before it:
//  - LLViewerFolderType::lookupIconName() (llviewerfoldertype.cpp): every
//    folder icon goes through it (the inventory tree, folder links, the
//    gallery, the thumbnail window).
//  - LLFolderBridge::getFolderIcon() (llinventorybridge.cpp) and the gallery's
//    folder item (llinventorygallery.cpp): the #Charis folder by its name.
//    (A link to the folder keeps the plain icon in the tree.)
// PlusInvFolderIcons (settings_plus.xml, on by default; Preferences > Charis) turns
// it off: the official icons come back, in the open inventory windows at once.
namespace plus::invfoldericons
{
    // Our icon's name for this folder type, or nullptr for the official one
    // (switched off, or a type we leave as it is: the marketplace folders).
    const std::string* iconName(LLFolderType::EType type, bool is_open);

    // Our own #Charis folder (a plain folder by type, so it is picked by name, and only
    // the one in the inventory root, where the AO store makes it): its icon, or nullptr
    // for any other folder or when the icons are switched off. folder_id is the category.
    const std::string* charisFolderIconName(LLFolderType::EType type, const LLUUID& folder_id, bool is_open);
}

#endif // PLUS_INVFOLDERICONS_H
