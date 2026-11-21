/**
 * @file rmlmenu.h
 * @brief The official context menu (LLContextMenu) opened from an RmlUi window (research/387 2.3, 3.2).
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

// A window that wants its own right-click menu (the radar's 18 actions, a "remove region" menu) keeps a ContextMenu
// as a member and opens an XUI menu file (skins/default/xui/<lang>/menu_*.xml, a <context_menu>) at the mouse:
//
//   afterLoad:     host->listen(list, "mousedown");                      // the right press is a mousedown, button 1
//   onEvent:       if (event.GetParameter<int>("button", 0) == 1) {
//                      event.StopPropagation();                         // else the host offers the field's edit menu
//                      (select the pressed row here, as the XUI does, then:)
//                      LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;      // the menu's functions,
//                      registrar.add("My.Action", ...);                                  // alive while it is made
//                      LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable;  ...
//                      mMenu.create("menu_my.xml");
//                      mMenu.open(*host, Rml::Vector2f(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0)));
//                  }
//   destructor:    mMenu.hide();                                          // before the window's members go
//
// The menu is shown inside the press (the XUI does the same: a right mouse up that did not move keeps it open).
// A menu with no functions of its own (the text editor menu's Edit.* act on gEditMenuHandler) is one call:
// show(host, "menu_text_editor.xml", point). It opens on the window the host is in, a detached one too.

#ifndef PLUS_RMLMENU_H
#define PLUS_RMLMENU_H

#include "rmlcommon.h"

#include "llhandle.h"

#include <string>

class LLContextMenu;
class LLRmlView;

namespace plus::rmlui
{
    class ContextMenu
    {
    public:
        ContextMenu() = default;
        ~ContextMenu() { hide(); }
        ContextMenu(const ContextMenu&) = delete;
        ContextMenu& operator=(const ContextMenu&) = delete;

        // Makes the menu from file (a plain file name of the xui menus; a name with a folder or ".." is refused)
        // and closes the one made before. The window's callback registrars are open around this call. Null when it
        // could not be made.
        LLContextMenu* create(const std::string& file);
        // Shows or hides an item of the created menu by its name (before open()); nothing for a name it has not.
        void setItemVisible(const std::string& name, bool visible);
        // Opens the created menu at a point of the host's document (context pixels, y down). True when it is shown.
        bool open(LLRmlView& host, Rml::Vector2f context_point);
        // create() and open().
        bool show(LLRmlView& host, const std::string& file, Rml::Vector2f context_point);
        // Closes the menu and lets it go (safe to call when none is open).
        void hide();
        bool isOpen() const;

    private:
        LLHandle<LLContextMenu> mMenu;
    };
}

#endif // PLUS_RMLMENU_H
