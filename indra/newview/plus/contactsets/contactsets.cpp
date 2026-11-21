/**
 * @file contactsets.cpp
 * @brief Firestorm's contact sets (plus/contactsets, research/333): the login, a display name's change, the
 *        file's reading for the bringing over of settings, the windows and the questions.
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

#include "contactsets.h"

#include "addtosetrml.h"

#include "fsfloateraddtocontactset.h"
#include "fsfloatercontactsetconfiguration.h"
#include "lggcontactsets.h"
#include "plus.h"
#include "rlv/rlvhooks.h"
#include "plus/safemode/safemode.h"

#include "llfile.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llslurl.h"
#include "llstartup.h"
#include "llviewercontrol.h"

namespace
{
    // A real file is well under a megabyte (tens of thousands of people); read whole on the main thread.
    constexpr S64 MAX_FILE_BYTES = 8 << 20;
    // <llsd><map><set><map><friends><map>: 6 levels; more is not a contact sets file.
    constexpr int MAX_FILE_DEPTH = 16;

    void init()
    {
        // Firestorm's names (cs-people and cs-color open them by these): lggcontactsets.h CS_FLOATER_*.
        // The "add to a set" window is the RmlUi one (addtosetrml.cpp, research/386). The XUI version (fsfloateraddtocontactset.cpp)
        // is frozen and is used in safe mode and with PlusRmlUiWindows off (chosen once, here: it takes effect at the next start).
        if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
        {
            LLFloaterReg::add(CS_FLOATER_ADD, "floater_plus_contact_set_add.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterAddToContactSet>);
        }
        else
        {
            plus::contactsets::addRmlAddToSetFloater(CS_FLOATER_ADD);
        }
        LLFloaterReg::add(CS_FLOATER_CONFIG, "floater_plus_contact_set_configuration.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterContactSetConfiguration>);
    }

    plus::FeatureRegistration sRegistration({ "contactsets", &init, {} });

    LLSD idsOf(const uuid_vec_t& ids)
    {
        LLSD list = LLSD::emptyArray();
        for (const LLUUID& id : ids)
        {
            if (id.notNull())
            {
                list.append(id);
            }
        }
        return list;
    }
}

namespace plus::contactsets
{
    void onLogin()
    {
        // The name cache's alias check first: what loadFromDisk()'s signal makes again (lists, tags) has the aliases.
        giveNameCacheTheAliases();
        LGGContactSets::getInstance()->loadFromDisk();
    }

    bool onDisplayNameUpdate(const LLUUID& agent_id, const LLSD& args)
    {
        if (!LGGContactSets::instanceExists() || !LGGContactSets::instance().hasPseudonym(agent_id))
        {
            return false;
        }
        LLSD payload;
        payload["agent_id"] = agent_id;
        // The answer may come late (a notice stays): the sets are looked up then, not held.
        LLNotificationsUtil::add("DisplayNameUpdateRemoveAlias", args, payload, [](const LLSD& notification, const LLSD& response)
        {
            return LGGContactSets::instanceExists() && LGGContactSets::instance().callbackAliasReset(notification, response);
        });
        return true;
    }

    ReadResult readFile(const std::string& path, LLSD& data)
    {
        data.clear();
        if (path.empty() || !LLFile::isfile(path))
        {
            return ReadResult::missing;
        }
        const S64 size = LLFile::size(path);
        if (size < 0 || size > MAX_FILE_BYTES || !plus::xmlNestingOk(path, MAX_FILE_DEPTH))
        {
            return ReadResult::unreadable;
        }
        llifstream in(path.c_str());
        if (!in.is_open())
        {
            return ReadResult::unreadable;
        }
        if (LLSDSerialize::fromXML(data, in) == LLSDParser::PARSE_FAILURE || !data.isMap())
        {
            data.clear();
            return ReadResult::unreadable;
        }
        return ReadResult::ok;
    }

    FileContent fileContent(const std::string& path)
    {
        LLSD data;
        switch (readFile(path, data))
        {
            case ReadResult::missing:
                return FileContent::missing;
            case ReadResult::unreadable:
                return FileContent::unreadable;
            case ReadResult::ok:
                break;
        }
        for (const auto& [key, value] : llsd::inMap(data))
        {
            if (key == CS_SET_EXTRA_AVS || key == CS_SET_PSEUDONYM)
            {
                if (value.isMap() && value.size() > 0)
                {
                    return FileContent::has_data;
                }
            }
            else if (key != CS_GLOBAL_SETTINGS && key != CS_SET_ALL_SETS && key != CS_SET_NO_SETS && !key.empty())
            {
                return FileContent::has_data;   // a set (with or without people: its name and colour are the user's)
            }
        }
        return FileContent::empty;
    }

    void reloadAfterImport()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || !LGGContactSets::instanceExists())
        {
            return;     // read at the login
        }
        LGGContactSets::instance().loadFromDisk();
    }

    bool namesShown(const uuid_vec_t& ids)
    {
        return !plus::rlv::enabled() || plus::rlv::canShowNames(ids);
    }

    void showAddToSet(const uuid_vec_t& ids)
    {
        if (ids.empty() || !namesShown(ids))
        {
            return;     // the window names them
        }
        if (ids.size() == 1)
        {
            if (ids.front().notNull())
            {
                LLFloaterReg::showInstance(CS_FLOATER_ADD, LLSD(ids.front()), true);
            }
            return;
        }
        LLFloaterReg::showInstance(CS_FLOATER_ADD, idsOf(ids), true);
    }

    void showMoveToSet(const uuid_vec_t& ids, const std::string& source_set)
    {
        // A set of the user's ("All Sets" and the like are never in the map of sets).
        if (ids.empty() || !namesShown(ids) || !LGGContactSets::getInstance()->isValidSet(source_set))
        {
            return;
        }
        LLSD data;
        data["operation"] = "move";
        data["source_set"] = source_set;
        if (ids.size() == 1)
        {
            data["id"] = ids.front();
        }
        else
        {
            data["ids"] = idsOf(ids);
        }
        LLFloaterReg::showInstance(CS_FLOATER_ADD, data, true);
    }

    LLFloater* showSetConfig(const std::string& set_name, LLView* origin)
    {
        if (!LGGContactSets::getInstance()->isValidSet(set_name))
        {
            return nullptr;
        }
        FSFloaterContactSetConfiguration* floater =
            LLFloaterReg::showTypedInstance<FSFloaterContactSetConfiguration>(CS_FLOATER_CONFIG, LLSD(set_name), true);
        if (floater)
        {
            floater->setFrustumOrigin(origin);
        }
        return floater;
    }

    void askAddSet()
    {
        LLNotificationsUtil::add("AddNewContactSet", LLSD(), LLSD(), &LGGContactSets::handleAddContactSetCallback);
    }

    void askRemoveSet(const std::string& set_name)
    {
        if (!LGGContactSets::getInstance()->isValidSet(set_name))
        {
            return;
        }
        LLSD payload, args;
        args["SET_NAME"] = plus::safeText(set_name);    // the user's words, shown in a text that makes links
        payload["contact_set"] = set_name;
        LLNotificationsUtil::add("RemoveContactSet", args, payload, &LGGContactSets::handleRemoveContactSetCallback);
    }

    void askRemoveFromSet(const uuid_vec_t& ids, const std::string& set_name)
    {
        if (ids.empty() || set_name.empty())
        {
            return;
        }
        LLSD payload, args;
        args["SET_NAME"] = plus::safeText(set_name);
        args["TARGET"] = ids.size() > 1 ? llformat("%d", (S32)ids.size()) : LLSLURL("agent", ids.front(), "about").getSLURLString();
        payload["contact_set"] = set_name;
        payload["ids"] = idsOf(ids);
        LLNotificationsUtil::add(ids.size() > 1 ? "RemoveContactsFromSet" : "RemoveContactFromSet", args, payload,
                                 &LGGContactSets::handleRemoveAvatarFromSetCallback);
    }

    void askSetPseudonym(const uuid_vec_t& ids)
    {
        if (ids.empty())
        {
            return;
        }
        LLSD payload, args;
        if (ids.size() == 1)
        {
            args["AVATAR"] = LLSLURL("agent", ids.front(), "about").getSLURLString();
            payload["id"] = ids.front();
        }
        else
        {
            args["COUNT"] = llformat("%d", (S32)ids.size());
            payload["ids"] = idsOf(ids);
        }
        LLNotificationsUtil::add(ids.size() > 1 ? "SetAvatarPseudonymMultiple" : "SetAvatarPseudonym", args, payload,
                                 &LGGContactSets::handleSetAvatarPseudonymCallback);
    }
}
