/**
 * @file contactsets.h
 * @brief Firestorm's contact sets (plus/contactsets, research/333): the viewer's side of them. The data is
 *        LGGContactSets (lggcontactsets.h, Firestorm's class); this is what the official files and the other
 *        features call: the login, a display name's change, the bringing over of settings and the windows.
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

#ifndef PLUS_CONTACTSETS_H
#define PLUS_CONTACTSETS_H

#include "lluuid.h"

#include <string>

class LLFloater;
class LLSD;
class LLView;

// All main thread.
namespace plus::contactsets
{
    // In each account's folder (Firestorm's name: its file is read as it is, and the bringing over of
    // settings copies it).
    constexpr char FILE_NAME[] = "settings_friends_groups.xml";

    // llstartup.cpp, once the friends are known: reads the account's contact sets and gives the name cache
    // the aliases (LLAvatarNameCache::setCustomNameCheckCallback).
    void onLogin();

    // llviewerdisplayname.cpp: agent_id's display name changed (args: OLD_NAME, SLID, NEW_NAME). With an
    // alias of ours for them, asks whether to remove it (DisplayNameUpdateRemoveAlias) and returns true: the
    // official notice (DisplayNameUpdate) is not shown then. False otherwise, and nothing is done.
    bool onDisplayNameUpdate(const LLUUID& agent_id, const LLSD& args);

    // ---- the file (lggcontactsets.cpp and the bringing over of settings, plus/importsettings) ----------

    enum class ReadResult
    {
        missing,        // no file there
        unreadable,     // there, but too big, nested too deep, not XML or not a map: never read into the sets
        ok,             // data is the file's map (its values are still to be checked: importFromLLSD())
    };
    // Reads a contact sets file (LLSD XML) whole, after checking its size and depth.
    ReadResult readFile(const std::string& path, LLSD& data);

    enum class FileContent
    {
        missing,
        unreadable,
        empty,          // no set, no one who is not a friend, no alias (its default colour only)
        has_data,
    };
    // What a contact sets file holds. The bringing over of settings replaces our file only when it is
    // empty and the source's has data (research/333 3).
    FileContent fileContent(const std::string& path);

    // After the bringing over of settings replaced the file of the account logged in: read again, the
    // windows and lists told (the signal). Nothing before the login (it is read then).
    void reloadAfterImport();

    // Gives the name cache the aliases (lggcontactsets.cpp; onLogin() calls it).
    void giveNameCacheTheAliases();

    // ---- the windows (Firestorm's LLAvatarActions::addToContactSet / moveToContactSet, which are not in
    // this viewer's official LLAvatarActions) ----------------------------------------------------------

    // false when RLV @shownames hides the name of one of these avatars (true without RLVa): the windows and
    // the menus that name them are not offered then.
    bool namesShown(const uuid_vec_t& ids);
    // "Add to a contact set" (llviewermenu.cpp, llpanelpeoplemenus.cpp, llpanelprofile.cpp, peopletab.cpp):
    // the window to choose the set. Nothing for no ids or while @shownames hides one of them (namesShown()).
    void showAddToSet(const uuid_vec_t& ids);
    // "Move to another set": as showAddToSet, and the avatars leave source_set once added. Nothing when
    // source_set is not one of the user's sets.
    void showMoveToSet(const uuid_vec_t& ids, const std::string& source_set);
    // A set's settings (its colour, notices, autoresponses, the colours' switches). origin: the view the
    // window's cone points to (null: none). Null when set_name is not a set.
    LLFloater* showSetConfig(const std::string& set_name, LLView* origin);

    // ---- the questions (Firestorm's notifications; the answers go to LGGContactSets' handlers) ----------

    // Asks for a new set's name (AddNewContactSet).
    void askAddSet();
    // Asks before removing a set (RemoveContactSet).
    void askRemoveSet(const std::string& set_name);
    // Asks before taking ids out of set_name (RemoveContactFromSet / RemoveContactsFromSet).
    void askRemoveFromSet(const uuid_vec_t& ids, const std::string& set_name);
    // Asks for an alias of ids (SetAvatarPseudonym / SetAvatarPseudonymMultiple).
    void askSetPseudonym(const uuid_vec_t& ids);
}

#endif // PLUS_CONTACTSETS_H
