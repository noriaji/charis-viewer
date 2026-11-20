/**
 * @file look.h
 * @brief Looks: colour grading of the finished image with a LUT, picked from
 *        the Charis menu (research/102-community-looks.md in the sl-viewer
 *        repository, stage 1a).
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

#ifndef PLUS_LOOK_LOOK_H
#define PLUS_LOOK_LOOK_H

#include <string>
#include <vector>

class LLRenderTarget;

namespace plus::look
{
    // A look of the folders, as the lists show it (the Charis menu, the photo tools).
    struct Entry
    {
        std::string id;     // what PlusLook holds: the shipped folder's name, or "user:" and the folder's or file's name
        std::string label;  // look.xml's name in the UI's language, else the folder's name, else the file's name without its extension
        bool        user = false;
        std::string group;  // a shipped look's look.xml group (portrait, street, film, mood, ...); empty: none, and for a user's look
    };

    // The shipped looks in their order, then the user's in the order of their names. The folders are
    // read the first time and again by reload(). Main thread.
    std::vector<Entry> entries();
    // The group submenus of Charis > Look (menu_plus.xml's LookGroup_<id>) in the menu's order, with their labels in the
    // UI's language: a look whose group is not here is in the Look menu itself. Empty while the menu is not built.
    struct Group
    {
        std::string id;
        std::string label;
    };
    std::vector<Group> menuGroups();
    // Whether a look of the folders has this id (a look with no name in look.xml still has its folder's).
    bool exists(const std::string& id);
    // The label of the look with this id; empty when no look of the folders has it.
    std::string labelOf(const std::string& id);
    // Reads the folders again: the menu is rebuilt and the LUT in use read again at the next idle
    // (the menu's "Reload the Looks" does the same).
    void reload();
    // Opens the user's look folder (made when missing) in the file manager.
    void openUserFolder();
    // Counts up with every read of the folders, so a window knows to fill its list again.
    unsigned generation();
    // The LUT of the look PlusLook names made now, not at the next idle as after a change of the setting:
    // for a picture taken in the same idle right after PlusLook was set (the presets' pictures of the
    // photo tools). Main thread, outside a frame (it makes a texture). Nothing when it is made already.
    void prepareSelectedNow();

    // The <plus> hook in LLPipeline::renderFinalize(), after anti-aliasing
    // and before the final pass: the image is tone mapped and finished, the
    // HUD and the UI come after it. Grades source into target (the same size)
    // with the selected look. True when it drew: the caller swaps the two.
    // False leaves source as the image: no look selected (PlusLook empty, the
    // default), "only in snapshots" outside a snapshot, a snapshot without
    // post processing, a backend that cannot draw it (GL, LLRHI_LOOK=0), or a
    // LUT that could not be read. Main thread.
    bool render(LLRenderTarget* source, LLRenderTarget* target);
}

#endif // PLUS_LOOK_LOOK_H
