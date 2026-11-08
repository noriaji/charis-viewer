/**
 * @file privacy.cpp
 * @brief Keeps secrets and Charis' own settings out of the ViewerStats report.
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

#include "privacy.h"

#include "lldir.h"
#include "llfile.h"
#include "llsd.h"
#include "llsdserialize.h"
#include "llstring.h"
#include "llviewercontrol.h"

#include <set>
#include <string>
#include <vector>

namespace plus::privacy
{
    namespace
    {
        constexpr char SEND_SETTINGS[] = "PlusSendSettingsStats";

        // Parts of a name that mark a secret, compared in lower case. Broad on
        // purpose: leaving out a harmless setting only loses a statistic.
        const char* const SECRET_WORDS[] = { "key", "token", "secret", "password", "passwd", "api" };

        // Settings that hold secrets but whose names do not say so.
        // UserLoginInfo: "--login first last password" puts the password
        // there as typed (research/130 H2). Also left out as an LLSD setting.
        const char* const SECRET_NAMES[] = { "UserLoginInfo" };

        // Settings of the Charis features: every name app_settings/settings_plus.xml declares
        // (not all of them begin with Plus: RenderScreenSpaceReflection*, MiniMapShowChatRange),
        // and, should that file not be read, the names that begin so.
        constexpr char OWN_SETTINGS_FILE[] = "settings_plus.xml";
        const char* const OWN_PREFIXES[] = { "Plus", "FS" };

        // Settings Charis added to the official app_settings/settings.xml and
        // settings_per_account.xml (neither in settings_plus.xml nor named Plus*/FS*).
        // Fixed here, not found by comparing files at run time: the official set of
        // names is not shipped. tools/predeliver.py (c) checks that every name the
        // official files have gained since the last official release is listed.
        // Also the retired Charis settings without such a name (plus.cpp
        // RETIRED_SETTINGS), which an older settings.xml can still hold.
        const char* const OWN_IN_OFFICIAL_FILES[] = {
            "RenderOITMode",            // order-independent transparency (Vulkan)
            "RenderOITSnapshotFull",
            "RenderGrayTextureWireframe",   // wireframe instead of the gray stand-in texture (llgraywire)
            "MiniMapShowVoiceRange",    // retired 2026-10-16
            "MiniMapVoiceRangeMeters",
        };

        // RenderTonemapType is official (0 = Khronos Neutral, 1 = ACES); Charis adds
        // 2 = Reinhard, 3 = Hable, 4 = Hejl. A Charis value is left out (not rounded
        // to 0 or 1, which would report a choice the user did not make).
        constexpr char TONEMAP_TYPE[] = "RenderTonemapType";
        constexpr S32 OFFICIAL_TONEMAP_TYPES = 2;

        // Names of Charis windows, panels and commands in the UI counts
        // (body["ui"], LLUIUsage), compared in lower case. LLUIUsage has
        // turned the dots of "Plus.Presets.Apply" into "_". "fs_", "poser",
        // "pose_": the poser from Firestorm (fs_poser, Poser.*, Pose.Menu).
        const char* const OWN_UI_PREFIXES[] = { "plus", "panel_plus", "floater_plus", "menu_plus", "fs_", "poser", "pose_" };

        bool isSecret(const std::string& name)
        {
            for (const char* secret : SECRET_NAMES)
            {
                if (name == secret)
                {
                    return true;
                }
            }
            std::string lower(name);
            LLStringUtil::toLower(lower);
            for (const char* word : SECRET_WORDS)
            {
                if (lower.find(word) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        // The names settings_plus.xml declares, read once (the statistics are sent twice a session at most).
        const std::set<std::string>& ownNames()
        {
            static std::set<std::string> sNames;
            static bool sRead = false;
            if (!sRead)
            {
                sRead = true;
                const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, OWN_SETTINGS_FILE);
                llifstream in(path.c_str());
                LLSD settings;
                if (in.is_open() && LLSDSerialize::fromXML(settings, in) != LLSDParser::PARSE_FAILURE && settings.isMap())
                {
                    for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
                    {
                        sNames.insert(it->first);
                    }
                }
                else
                {
                    LL_WARNS("Plus") << "ViewerStats: cannot read " << path << "; Charis settings told by their names only" << LL_ENDL;
                }
            }
            return sNames;
        }

        bool isOwn(const std::string& name)
        {
            if (ownNames().count(name))
            {
                return true;
            }
            for (const char* own : OWN_IN_OFFICIAL_FILES)
            {
                if (name == own)
                {
                    return true;
                }
            }
            for (const char* prefix : OWN_PREFIXES)
            {
                if (LLStringUtil::startsWith(name, prefix))
                {
                    return true;
                }
            }
            return false;
        }

        // An official setting holding a value only Charis has.
        bool isOwnValue(const std::string& name, const LLSD& value)
        {
            if (name == TONEMAP_TYPE)
            {
                const S32 type = value.asInteger();
                return type < 0 || type >= OFFICIAL_TONEMAP_TYPES;
            }
            return false;
        }

        // Only the on/off, number and colour settings are sent: the statistics
        // need nothing else. Every other type can hold what the user typed or
        // where they were: String (NextLoginLocation, audio device names, file
        // paths, URLs; LLControlGroup::asLLSD() already skips them, kept out
        // here too in case that changes), LLSD (login names and password,
        // URIs, device GUIDs), Vector3D (CameraPosOnLogout, FocusPosOnLogout:
        // where you logged out), Vector3, Quaternion, Rect. A setting the
        // group does not know is not sent either.
        bool isFreeForm(LLControlGroup& group, const std::string& name)
        {
            LLControlVariable* control = group.getControl(name);
            if (!control)
            {
                return true;
            }
            switch (control->type())
            {
                case TYPE_BOOLEAN:
                case TYPE_S32:
                case TYPE_U32:
                case TYPE_F32:
                case TYPE_COL3:
                case TYPE_COL4:
                    return false;
                default:
                    return true;
            }
        }

        // Returns how many settings it removed.
        size_t filterGroup(LLSD& settings, LLControlGroup& group)
        {
            if (!settings.isMap())
            {
                return 0;
            }

            std::vector<std::string> names;
            for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
            {
                if (isSecret(it->first) || isOwn(it->first) || isOwnValue(it->first, it->second) || isFreeForm(group, it->first))
                {
                    names.push_back(it->first);
                }
            }
            for (const std::string& name : names)
            {
                settings.erase(name);
            }
            return names.size();
        }

        bool isOwnUI(const std::string& name)
        {
            std::string lower(name);
            LLStringUtil::toLower(lower);
            for (const char* prefix : OWN_UI_PREFIXES)
            {
                if (LLStringUtil::startsWith(lower, prefix))
                {
                    return true;
                }
            }
            return false;
        }

        // Removes the Charis names from a map of UI counts, and from the maps
        // in it down to depth levels (ui.controls is window, then control).
        size_t filterUINames(LLSD& map, int depth)
        {
            if (!map.isMap())
            {
                return 0;
            }

            size_t removed = 0;
            std::vector<std::string> names;
            for (LLSD::map_iterator it = map.beginMap(); it != map.endMap(); ++it)
            {
                if (isOwnUI(it->first))
                {
                    names.push_back(it->first);
                }
                else if (depth > 1)
                {
                    removed += filterUINames(it->second, depth - 1);
                }
            }
            for (const std::string& name : names)
            {
                map.erase(name);
            }
            return removed + names.size();
        }

        // body["ui"] (LLUIUsage::asLLSD()): commands, controls, floaters and
        // panels with how often they were used (research/130 L3).
        size_t filterUI(LLSD& ui)
        {
            if (!ui.isMap())
            {
                return 0;
            }
            size_t removed = 0;
            for (const char* part : { "commands", "floaters", "panels" })
            {
                if (ui.has(part))
                {
                    removed += filterUINames(ui[part], 1);
                }
            }
            if (ui.has("controls"))
            {
                removed += filterUINames(ui["controls"], 2);
            }
            return removed;
        }

        bool sendSettings()
        {
            // Read each time (twice a session at most). Missing: on, the default.
            LLControlVariable* control = gSavedSettings.getControl(SEND_SETTINGS);
            return !control || control->getValue().asBoolean();
        }
    }

    void filterStats(LLSD& body)
    {
        if (!body.isMap())
        {
            return;
        }

        if (body.has("ui"))
        {
            const size_t removed = filterUI(body["ui"]);
            LL_INFOS("Plus") << "ViewerStats: " << removed << " Charis UI names left out" << LL_ENDL;
        }

        if (!body.has("preferences"))
        {
            return;
        }

        if (!sendSettings())
        {
            body.erase("preferences");
            LL_INFOS("Plus") << "ViewerStats: settings left out (" << SEND_SETTINGS << " off)" << LL_ENDL;
            return;
        }

        LLSD& preferences = body["preferences"];
        size_t removed = 0;
        if (preferences.has("settings"))
        {
            removed += filterGroup(preferences["settings"], gSavedSettings);
        }
        if (preferences.has("settings_per_account"))
        {
            removed += filterGroup(preferences["settings_per_account"], gSavedPerAccountSettings);
        }
        LL_INFOS("Plus") << "ViewerStats: " << removed << " settings left out (secrets, text and other free-form settings, Charis settings)" << LL_ENDL;
    }
}
