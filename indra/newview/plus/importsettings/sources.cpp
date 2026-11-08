/**
 * @file sources.cpp
 * @brief The viewers on this computer that settings can be brought over from
 *        (research/143).
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

// Where they are looked for:
// - %APPDATA%\<folder> of the viewers in KNOWN: the official viewer, Firestorm
//   (64 and 32 bit, VR, OpenSim) and other viewers built on the official one,
//   which keep their settings the same way.
// - %APPDATA%\CharisViewer, when this install is a portable one.
// - The folders other Charis Viewer installs noted in the registry
//   (HKEY_CURRENT_USER\Software\CharisViewer\UserFolders: one value per folder,
//   named by its path, the date of its last start as the data). A portable
//   install keeps its folder next to itself, where nothing else would find it.
//   Folders that are gone are taken out of the list.
// A folder counts only when it has user_settings\settings.xml.

#include "llviewerprecompiledheaders.h"

#include "importsettings.h"
#include "independence/independence.h"

#include "fsyspath.h"
#include "lldate.h"
#include "lldir.h"
#include "llstring.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace
{
    using namespace plus::importsettings;

    struct Known
    {
        const char* folder;     // under %APPDATA%
        Family family;
        const char* name;
    };

    const Known KNOWN[] = {
        { plus::independence::OFFICIAL_APP_DIR_NAME, Family::OFFICIAL,  "Second Life" },
        { "Firestorm_x64",                           Family::FIRESTORM, "Firestorm" },
        { "FirestormVR_x64",                         Family::FIRESTORM, "Firestorm VR" },
        { "FirestormOS_x64",                         Family::FIRESTORM, "Firestorm (OpenSim)" },
        { "Firestorm",                               Family::FIRESTORM, "Firestorm (32-bit)" },
        { "FirestormOS",                             Family::FIRESTORM, "Firestorm (OpenSim, 32-bit)" },
        { "BlackDragon",                             Family::OTHER,     "Black Dragon" },
        { "Alchemy",                                 Family::OTHER,     "Alchemy" },
        { "Catznip",                                 Family::OTHER,     "Catznip" },
        { "Kokua",                                   Family::OTHER,     "Kokua" },
    };

    constexpr char CHARIS_NAME[] = "Charis Viewer";

#if LL_WINDOWS
    constexpr wchar_t REGISTRY_KEY[] = L"Software\\CharisViewer\\UserFolders";
#endif

    std::string utf8(const fs::path& path)
    {
        return fsyspath(path).string();
    }

    // For comparing folders: lower case, backslashes, no slash at the end.
    std::string folderKey(const std::string& path)
    {
        std::string key = path;
        LLStringUtil::replaceChar(key, '/', '\\');
        while (key.size() > 3 && key.back() == '\\')
        {
            key.pop_back();
        }
        LLStringUtil::toLower(key);
        return key;
    }

    fs::path settingsFile(const fs::path& root)
    {
        return root / "user_settings" / "settings.xml";
    }

    bool hasSettings(const fs::path& root)
    {
        std::error_code ec;
        return fs::is_regular_file(settingsFile(root), ec);
    }

    std::time_t lastUsed(const fs::path& root)
    {
        std::error_code ec;
        const fs::file_time_type written = fs::last_write_time(settingsFile(root), ec);
        if (ec)
        {
            return 0;
        }
        return std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
    }

    // The folders other Charis Viewer installs noted. Those that are gone are taken out.
    std::vector<std::string> registeredFolders()
    {
        std::vector<std::string> folders;
#if LL_WINDOWS
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return folders;
        }
        std::vector<std::wstring> gone;
        for (DWORD index = 0; ; ++index)
        {
            wchar_t name[1024];
            DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            {
                break;  // ERROR_NO_MORE_ITEMS, or a name too long to be one of ours
            }
            const std::wstring wide(name, length);
            const std::string folder = ll_convert<std::string>(wide);
            std::error_code ec;
            if (fs::exists(fsyspath(folder), ec))
            {
                folders.push_back(folder);
            }
            else if (!ec)
            {
                gone.push_back(wide);
            }
        }
        for (const std::wstring& name : gone)
        {
            RegDeleteValueW(key, name.c_str());
        }
        RegCloseKey(key);
#endif
        return folders;
    }

    Family familyOfFolder(const fs::path& root, const std::set<std::string>& charis_folders, std::string& name)
    {
        const std::string leaf = utf8(root.filename());
        for (const Known& known : KNOWN)
        {
            if (LLStringUtil::compareInsensitive(leaf, known.folder) == 0)
            {
                name = known.name;
                return known.family;
            }
        }
        std::error_code ec;
        const bool portable_charis = LLStringUtil::compareInsensitive(leaf, "UserData") == 0
                                     && fs::is_regular_file(root.parent_path() / ".portable", ec);
        if (LLStringUtil::compareInsensitive(leaf, plus::independence::APP_DIR_NAME) == 0
            || portable_charis
            || charis_folders.count(folderKey(utf8(root))))
        {
            name = CHARIS_NAME;
            return Family::CHARIS;
        }
        // A drive or share root has no leaf name: show "D:" (or the share) instead of nothing.
        name = leaf.empty() ? utf8(root.root_name()) : leaf;
        if (name.empty())
        {
            name = utf8(root);
        }
        return Family::OTHER;
    }
}

std::vector<plus::importsettings::Source> plus::importsettings::findSources()
{
    std::vector<Source> found;
    std::set<std::string> seen = { folderKey(gDirUtilp->getOSUserAppDir()) };

    auto add = [&](const fs::path& root, Family family, const std::string& name)
    {
        if (!seen.insert(folderKey(utf8(root))).second || !hasSettings(root))
        {
            return;
        }
        Source source;
        source.family = family;
        source.name = name;
        source.root = utf8(root);
        source.last_used = lastUsed(root);
        found.push_back(source);
    };

    const std::string user_dir = gDirUtilp->getOSUserDir();
    if (!user_dir.empty())
    {
        add(fsyspath(user_dir) / plus::independence::APP_DIR_NAME, Family::CHARIS, CHARIS_NAME);
    }
    for (const std::string& folder : registeredFolders())
    {
        add(fsyspath(folder), Family::CHARIS, CHARIS_NAME);
    }
    if (!user_dir.empty())
    {
        for (const Known& known : KNOWN)
        {
            add(fsyspath(user_dir) / known.folder, known.family, known.name);
        }
    }

    std::stable_sort(found.begin(), found.end(),
                     [](const Source& a, const Source& b) { return a.last_used > b.last_used; });
    return found;
}

bool plus::importsettings::sourceFromFolder(const std::string& folder, Source& source)
{
    fs::path root = fsyspath(folder);
    {
        // "D:\Viewers\Fire\" -> "D:\Viewers\Fire", so the leaf name and the path matching work.
        // A drive root keeps its separator: "D:" alone would mean D:'s current folder.
        fs::path::string_type text = root.native();
        const size_t keep = root.root_path().native().size();
        while (text.size() > keep && (text.back() == '\\' || text.back() == '/'))
        {
            text.pop_back();
        }
        root = fs::path(text);
    }
    if (!hasSettings(root) && LLStringUtil::compareInsensitive(utf8(root.filename()), "user_settings") == 0)
    {
        root = root.parent_path();
    }
    if (!hasSettings(root) || folderKey(utf8(root)) == folderKey(gDirUtilp->getOSUserAppDir()))
    {
        return false;
    }

    std::set<std::string> charis_folders;
    for (const std::string& registered : registeredFolders())
    {
        charis_folders.insert(folderKey(registered));
    }
    source.family = familyOfFolder(root, charis_folders, source.name);
    source.root = utf8(root);
    source.last_used = lastUsed(root);
    return true;
}

void plus::importsettings::rememberThisInstall()
{
#if LL_WINDOWS
    const std::string folder = gDirUtilp->getOSUserAppDir();
    if (folder.empty())
    {
        return;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        LL_WARNS("ImportSettings") << "Could not note this install's folder in the registry" << LL_ENDL;
        return;
    }
    const std::wstring name = ll_convert<std::wstring>(folder);
    const std::wstring date = ll_convert<std::wstring>(LLDate::now().asString());
    RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(date.c_str()),
                   static_cast<DWORD>((date.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
#endif
}
