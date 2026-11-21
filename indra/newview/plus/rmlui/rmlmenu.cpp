/**
 * @file rmlmenu.cpp
 * @brief The official context menu opened from an RmlUi window (research/387 2.3, 3.2).
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

#include "llviewerprecompiledheaders.h"

#include "rmlmenu.h"

#include "rmlhost.h"

#include "llmenugl.h"
#include "lluictrlfactory.h"

namespace plus::rmlui
{
    LLContextMenu* ContextMenu::create(const std::string& file)
    {
        hide();
        // A file of the xui menus by its plain name (the names are the windows' own constants; refused anyway when one
        // would leave the folder).
        if (file.empty() || file.find_first_of("/\\:") != std::string::npos || file.find("..") != std::string::npos ||
            !LLMenuGL::sMenuContainer)
        {
            return nullptr;
        }
        LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
            file, LLMenuGL::sMenuContainer, LLMenuHolderGL::child_registry_t::instance());
        if (menu)
        {
            mMenu = menu->getHandle();
        }
        return menu;
    }

    void ContextMenu::setItemVisible(const std::string& name, bool visible)
    {
        if (LLContextMenu* menu = mMenu.get())
        {
            menu->setItemVisible(name, visible);
        }
    }

    bool ContextMenu::open(LLRmlView& host, Rml::Vector2f context_point)
    {
        LLContextMenu* menu = mMenu.get();
        if (!menu)
        {
            return false;
        }
        S32 local_x = 0;
        S32 local_y = 0;
        host.contextToLocal(context_point, local_x, local_y);
        S32 screen_x = 0;
        S32 screen_y = 0;
        host.localPointToScreen(local_x, local_y, &screen_x, &screen_y);
        menu->show(screen_x, screen_y, &host);      // screen units, as the XUI text editor opens its menu
        if (!menu->getVisible())
        {
            hide();     // nothing to show (no items): do not keep an invisible one
            return false;
        }
        return true;
    }

    bool ContextMenu::show(LLRmlView& host, const std::string& file, Rml::Vector2f context_point)
    {
        return create(file) && open(host, context_point);
    }

    void ContextMenu::hide()
    {
        if (LLContextMenu* menu = mMenu.get())
        {
            menu->hide();
            menu->die();
        }
        mMenu.markDead();
    }

    bool ContextMenu::isOpen() const
    {
        const LLContextMenu* menu = mMenu.get();
        return menu && menu->getVisible();
    }
}
