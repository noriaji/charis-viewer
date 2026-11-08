/**
 * @file invcache.cpp
 * @brief Clear the inventory cache: a button in Preferences > Advanced marks the
 *        account, and its inventory cache files are deleted at its next login.
 *
 * Ported from Firestorm's "Clear Inventory Cache" button (llfloaterpreference.cpp,
 * FS:Ansariel) and the marker file read in LLInventoryModel::loadSkeleton()
 * (FS:Zi, FIRE-22353); see ORIGIN.md.
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

// Why: a folder the server refuses to send in one piece (too many items, HTTP 403)
// is not saved to the cache, and a damaged cache can make folders look empty or
// items go missing. Loading the whole inventory from the server again is the
// first thing Second Life support asks for (research/144).
//
// How it runs:
//   the button (logged in) -> asks -> writes <cache>/<agent id>_DELETE_INV_GZ
//   next login, entering STATE_SEED_CAP_GRANTED (before the cache loads start:
//   the agent's in that state, the library's in STATE_INVENTORY_SEND)
//     -> marker there: delete <agent id>.inv.llsd(.gz) and the library's, then the marker
// A marker file, not a setting, as in Firestorm: logging out does not lose it, and
// a second viewer logged in to the same account does not write it back.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "llagent.h"
#include "lldir.h"
#include "llevents.h"
#include "llfile.h"
#include "llinventorymodel.h"
#include "lllogininstance.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "llviewernetwork.h"

#include <cctype>

namespace plus::invcache
{
    namespace
    {
        // Per grid, as the cache it marks (LLInventoryModel::getInvCacheAddres()): the main
        // grid's name stays as it was; another grid's has the grid's id (only letters, digits,
        // '-', '.' kept), so marking one grid's cache does not delete another's.
        std::string markerPath(const LLUUID& agent_id)
        {
            std::string name = agent_id.asString();
            if (!LLGridManager::getInstance()->isInProductionGrid())
            {
                std::string grid = utf8str_tolower(LLGridManager::getInstance()->getGridId());
                for (char& c : grid)
                {
                    const unsigned char u = (unsigned char)c;
                    if (!(std::isalnum(u) && u < 0x80) && c != '-' && c != '.')
                    {
                        c = '_';
                    }
                }
                name += "." + grid;
            }
            return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, name + "_DELETE_INV_GZ");
        }

        // The cache is saved as <name>.gz; <name> itself is an older form, or what is
        // left when the viewer stopped while unpacking it.
        void deleteCacheFiles(const LLUUID& owner_id, const char* what)
        {
            const std::string name = LLInventoryModel::getInvCacheAddres(owner_id);
            for (const std::string& file : { name, name + ".gz" })
            {
                if (LLFile::isfile(file))
                {
                    LL_INFOS("PlusInvCache") << "Deleting the " << what << " inventory cache: " << file << LL_ENDL;
                    LLFile::remove(file);
                }
            }
        }

        // The library's owner is set from the login response in STATE_INVENTORY_SEND, after
        // this runs, so it is read from the response here.
        LLUUID libraryOwner()
        {
            const LLSD owners = LLLoginInstance::getInstance()->getResponse()["inventory-lib-owner"];
            return owners.isArray() && owners.size() > 0 ? owners[0]["agent_id"].asUUID() : LLUUID::null;
        }

        bool onStartupState(const LLSD& info)
        {
            if (info["enum"].asInteger() != STATE_SEED_CAP_GRANTED || gAgentID.isNull())
            {
                return false;
            }
            const std::string marker = markerPath(gAgentID);
            if (!LLFile::isfile(marker))
            {
                return false;
            }
            deleteCacheFiles(gAgentID, "account's");
            // The library's too, as in Firestorm: missing environment settings and
            // library items come back with it.
            const LLUUID library_owner = libraryOwner();
            if (library_owner.notNull())
            {
                deleteCacheFiles(library_owner, "library's");
            }
            LLFile::remove(marker);
            return false;
        }

        void onClick()
        {
            // The cache is per account: before login there is no account to mark.
            if (gAgentID.isNull() || LLStartUp::getStartupState() < STATE_STARTED)
            {
                LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", LLTrans::getString("PlusInvCacheLoginFirst")));
                return;
            }
            const std::string marker = markerPath(gAgentID);   // this account on this grid
            LLNotificationsUtil::add("GenericAlertYesCancel",
                                     LLSD().with("MESSAGE", LLTrans::getString("PlusInvCacheConfirm")),
                                     LLSD(),
                                     [marker](const LLSD& notification, const LLSD& response)
                                     {
                                         if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                         {
                                             return;
                                         }
                                         LLFILE* fp = LLFile::fopen(marker, LLFILE_MODE("w"));
                                         if (!fp)
                                         {
                                             LL_WARNS("PlusInvCache") << "Could not write " << marker << LL_ENDL;
                                             LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", LLTrans::getString("PlusInvCacheNotWritten")));
                                             return;
                                         }
                                         LLFile::close(fp);
                                         LL_INFOS("PlusInvCache") << "The inventory cache is deleted at the next login: " << marker << LL_ENDL;
                                         LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", LLTrans::getString("PlusInvCacheMarked")));
                                     });
        }

        void init()
        {
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.InvCache.Clear",
                [](LLUICtrl*, const LLSD&) { onClick(); });
            LLEventPumps::instance().obtain("StartupState").listen("PlusInvCache", &onStartupState);
        }

        plus::FeatureRegistration sRegistration({ "invcache", &init });
    }
}
