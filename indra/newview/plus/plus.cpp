/**
 * @file plus.cpp
 * @brief "Plus" foundation: feature registry, settings, UI images and the
 *        "Plus" menu for features ported from other viewers.
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

#include "plus.h"

#include "plus/photomode/phototools.h"
#include "plus/loginfast/loginfast.h"
#include "plus/rlv/rlvhooks.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llevents.h"
#include "llfile.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llmenugl.h"
#include "llsettingssky.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "llthread.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "fsyspath.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <objbase.h>  // CoInitializeEx (left out by WIN32_LEAN_AND_MEAN)
#include <shellapi.h>
#else
#include "llprocess.h"
#endif

extern bool gSnapshotNoPost;    // llviewerdisplay.cpp: a snapshot taken without post-processing

namespace
{
    constexpr char PLUS_SETTINGS_FILE[] = "settings_plus.xml"; // in app_settings/
    constexpr char PLUS_MENU_FILE[]     = "menu_plus.xml";     // in skins/<skin>/xui/<lang>/
    constexpr char PLUS_MENU_NAME[]     = "Plus";              // placeholder menu in menu_viewer.xml

    struct UIImageDecl
    {
        std::string name;
        std::string file_name;
    };

    // Function-local statics: features register from static initializers in
    // other translation units, so these must exist before first use.
    std::vector<plus::Feature>& features()
    {
        static std::vector<plus::Feature> sFeatures;
        return sFeatures;
    }

    // Features whose init() or onUIReady() threw. Only their later hook is
    // skipped (no onUIReady after a failed init); whatever the hook registered
    // before it threw (floaters, menu callbacks, settings) stays.
    std::set<std::string>& failedFeatures()
    {
        static std::set<std::string> sFailed;
        return sFailed;
    }

    std::vector<UIImageDecl>& uiImages()
    {
        static std::vector<UIImageDecl> sImages;
        return sImages;
    }

    // Declares the controls in app_settings/settings_plus.xml in gSavedSettings,
    // or in gSavedPerAccountSettings for entries with "PerAccount" 1 (their
    // values are read at login, from the account's settings file).
    // This runs after the user's settings.xml was read, so a control the user
    // has already saved was declared from that file as an unknown control:
    // adopt it, giving it our default, comment and persistence while keeping
    // the user's value. Returns the names in the file.
    std::set<std::string> loadSettings()
    {
        std::set<std::string> names;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, PLUS_SETTINGS_FILE);
        llifstream infile(path.c_str());
        if (!infile.is_open())
        {
            LL_WARNS("Plus") << "Cannot open " << path << "; plus features will use undeclared settings" << LL_ENDL;
            return names;
        }

        LLSD settings;
        if (LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(settings, infile))
        {
            LL_WARNS("Plus") << "Cannot parse " << path << LL_ENDL;
            return names;
        }

        S32 declared = 0, adopted = 0;
        for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
        {
            const std::string& name = it->first;
            const LLSD& entry       = it->second;
            names.insert(name);

            const eControlType type = LLControlGroup::typeStringToEnum(entry["Type"].asString());
            if (type < 0 || type >= TYPE_COUNT)
            {
                LL_WARNS("Plus") << "Setting " << name << " has an unknown type; skipped" << LL_ENDL;
                continue;
            }

            const LLControlVariable::ePersist persist =
                (!entry.has("Persist") || entry["Persist"].asInteger()) ? LLControlVariable::PERSIST_NONDFT : LLControlVariable::PERSIST_NO;
            const bool        hidden  = entry.has("HideFromEditor") && entry["HideFromEditor"].asInteger();
            const std::string comment = entry["Comment"].asString();
            const LLSD&       value   = entry["Value"];
            LLControlGroup&   group   = (entry.has("PerAccount") && entry["PerAccount"].asInteger()) ? gSavedPerAccountSettings : gSavedSettings;

            LLControlVariable* control = group.getControl(name);
            if (!control)
            {
                group.declareControl(name, type, value, comment, persist, hidden);
                ++declared;
                continue;
            }

            if (!control->isType(type))
            {
                // An old value of another type in settings.xml (LLControlGroup cannot change
                // a control's type): not saved again, so ours is declared at the next start.
                LL_WARNS("Plus") << "Setting " << name << " already exists with another type; not saved, ours from the next start" << LL_ENDL;
                control->setPersist(LLControlVariable::PERSIST_NO);
                continue;
            }

            const LLSD current = control->getValue();
            control->setDefaultValue(value);
            if (!llsd_equals(current, value))
            {
                control->setValue(current);
            }
            control->setPersist(persist);
            control->setHiddenFromSettingsEditor(hidden);
            control->setComment(comment);
            ++adopted;
        }

        LL_INFOS("Plus") << "Settings: " << declared << " declared, " << adopted << " already saved by the user" << LL_ENDL;
        return names;
    }

    // Settings that Charis had once and dropped from settings_plus.xml. The
    // user's settings.xml keeps them: LLControlGroup::loadFromFile() declares a
    // name it does not know as PERSIST_ALWAYS, so that a newer viewer's
    // settings survive an older one, and saves it again at every quit. Ours
    // will never come back under these names, so they are let go: not saved
    // (gone from settings.xml at the next quit) and out of Debug Settings.
    // Global settings only (none of them was per account). Runs after
    // loadSettings(), so a name that came back into settings_plus.xml would
    // be ours again and is left alone.
    const char* const RETIRED_SETTINGS[] = {
        "PlusInvFinderMcpEnabled",      // 2026-10-18: one MCP server (PlusPoserMcp*)
        "PlusInvFinderMcpPort",
        "PlusInvFinderMcpKeepToken",
        "PlusCameraShotIn",             // 2026-10-20: camerashot's own buttons
        "PlusNudgeGainAt",              // 2026-10-18: nudge in whole steps
        "PlusNudgeGainLeft",
        "PlusHairSwayMaxAngle",         // 2026-10-15: hair sway in the skinning shaders
        "PlusVolumetricAlphaDepth",     // 2026-10-16: the shafts before the alpha
        "PlusGPUBenchmarkVulkanScale",  // 2026-10-19: ...NVIDIA and ...Other
        "MiniMapShowVoiceRange",        // 2026-10-16: the chat range circles (minimapchat) for the voice one
        "MiniMapVoiceRangeMeters",
        "PlusPhotoPresetThumbs",        // 2026-10-22: no sample pictures on the photo presets (tool tips instead)
        "PlusDetachButtonAll",          // 2026-10-23: the detach button follows PlusDetachAutoOnDrag
        "PlusTranslateDrawMinDraws",    // 2026-10-27: the translation thread retired (research/207 T1)
        "PlusTranslateDrawDownPercent",
    };

    void retireOldSettings(const std::set<std::string>& ours)
    {
        S32 retired = 0;
        for (const char* name : RETIRED_SETTINGS)
        {
            if (ours.count(name))
            {
                continue;
            }
            if (LLControlVariable* control = gSavedSettings.getControl(name))
            {
                control->setPersist(LLControlVariable::PERSIST_NO);
                control->setHiddenFromSettingsEditor(true);
                ++retired;
            }
        }
        if (retired)
        {
            LL_INFOS("Plus") << "Settings: " << retired << " old Charis settings left out of settings.xml" << LL_ENDL;
        }
    }

    // Development switches that were saved once and are "Persist" 0 now ("off
    // again at the next start"). loadSettings() keeps the value settings.xml
    // still has from an older Charis, so without this a switch left on there
    // would hold for one more session after the update (the hair not swaying,
    // diagnostic logging, navmesh dumps). Persist 0 also keeps them out of the
    // next save, so from then on they start at the default by themselves.
    // Not every Persist 0 setting: PlusGazeAllowEveryone is written into
    // settings.xml by hand for one session on purpose (its comment), and the
    // others were never saved by Charis.
    const char* const UNSAVED_SWITCHES[] = {
        "PlusHairSwayDetectOnly",       // 2026-10-21 (fix-settings4): Persist 0
        "PlusUiFrameLog",
        "PlusShadowDiagnostics",
        "PlusPathingDumpBlob",
        "PlusGazeDiag",                 // 2026-10-22 (fix-plus3): Persist 0
        "PlusAOLogTiming",
        "PlusSelfFirstLog",
        "PlusOcean",                    // 2026-10-31 (ocean-retire): retired, out of the UI and the levels
    };

    void resetUnsavedSwitches(const std::set<std::string>& ours)
    {
        for (const char* name : UNSAVED_SWITCHES)
        {
            if (!ours.count(name))
            {
                continue; // not ours (any more): left to retireOldSettings() or as is
            }
            LLControlVariable* control = gSavedSettings.getControl(name);
            if (control && !control->isPersisted() && !control->isDefault())
            {
                control->resetToDefault(true);
                LL_INFOS("Plus") << "Settings: " << name << " is not kept between starts; back to its default" << LL_ENDL;
            }
        }
    }

    // A login that failed (a wrong password) has already read that account's
    // settings_per_account.xml: STATE_LOGIN_CLEANUP reads it before the login
    // server answers. loadFromFile() sets only the names in a file, so a second
    // account logging in after it (same start) would keep the first one's values
    // for every setting its own file does not have (PlusGroupChatWhitelistOnly
    // refusing its group chats, the keywords, the AO, the protected folders) and,
    // once logged in, save them into its own file at quit (PERSIST_NONDFT). So
    // before each later attempt reads its file, the per-account settings go back
    // to their defaults. With the signal, as a value read from a file comes: the
    // cached controls and the features' listeners follow. The first attempt is
    // left as it was (nothing read yet: PerAccountSettingsFile is still empty).
    // Not handled: a name no part of this viewer declares (from a newer viewer's
    // file) is declared by loadFromFile() with the value it read as its default
    // and saved always, so it goes on into the next account's file.
    struct ResetToDefault : public LLControlGroup::ApplyFunctor
    {
        S32 reset = 0;
        void apply(const std::string&, LLControlVariable* control) override
        {
            if (control && !control->isDefault())
            {
                control->resetToDefault(true);
                ++reset;
            }
        }
    };

    bool onStartupState(const LLSD& info)
    {
        if (info["enum"].asInteger() == STATE_LOGIN_CLEANUP && !gSavedSettings.getString("PerAccountSettingsFile").empty())
        {
            ResetToDefault reset;
            gSavedPerAccountSettings.applyToAll(&reset);
            LL_INFOS("Plus") << "Settings: " << reset.reset
                             << " per-account settings of the last login attempt back to their defaults" << LL_ENDL;
        }
        return false;
    }

    void preloadUIImages()
    {
        LLUIImageList* images = LLUIImageList::getInstance();
        for (const UIImageDecl& decl : uiImages())
        {
            // Same call as LLUIImageList::initFromFile() makes for a textures.xml entry
            // without scale/clip; the image decodes with the normal texture updates.
            images->preloadUIImage(decl.name, decl.file_name, false, LLRect::null, LLRect::null, LLUIImage::SCALE_INNER);
        }
    }

    // Items of menu_plus.xml that Firestorm also has in its menu bar go where Firestorm
    // puts them, so a Firestorm user finds them without looking. The parent is the
    // name in menu_viewer.xml; after_item is the item it follows (empty: the first, below the
    // tear-off line if the menu has one). before_item (optional) is the item it goes in front of,
    // for a place that only has a separator (no name) above it; it is used when after_item is empty.
    // Anything not listed (or whose parent or neighbour is missing) goes to the
    // Charis menu.
    struct FsMenuPlace
    {
        const char* item;
        const char* parent;
        const char* after_item;
        const char* before_item = nullptr;
    };
    const FsMenuPlace FS_MENU_PLACES[] = {
        {"ProtectedFolders", "Me",            "NewInventoryWindow"},  // FS: Avatar, after New Inventory Window
        {"Poser",            "Avatar",        "Stop Animating My Avatar"}, // FS: Avatar, with Pose Stand
        {"ChatConsoleOn",    "Communicate",   "Block List"},          // FS: Comm, after Block List
        {"Radar",            "World",         ""},                    // FS: World, after Nearby Avatars (the top of the menu)
        {"AreaSearch",       "World",         "Environment"},         // FS: World, after Photo and Video / Environment
        {"LocalMesh",        "BuildTools",    "Upload"},              // FS: Build, after Upload
        {"RLV API",          "Advanced",      "", "Debug Settings"},  // FS: Advanced, below the Shortcuts separator, above Show Debug Settings (rlv)
    };

    // Index of the item called name in menu, -1 if none.
    S32 findMenuItemIndex(LLMenuGL* menu, const std::string& name)
    {
        for (U32 i = 0; i < menu->getItemCount(); ++i)
        {
            if (menu->getItem(i)->getName() == name)
            {
                return (S32)i;
            }
        }
        return -1;
    }

    // Puts item into the FS place listed for its name. false: not listed or the place is missing.
    bool placeAtFsPosition(LLMenuItemGL* item)
    {
        for (const FsMenuPlace& place : FS_MENU_PLACES)
        {
            if (item->getName() != place.item)
            {
                continue;
            }
            LLMenuGL* parent = gMenuBarView->findChildMenuByName(place.parent, true);
            if (!parent)
            {
                return false;
            }
            // The first: after the tear-off line a tear_off="true" menu has as its item 0
            // (LLMenuGL::setCanTearOff adds it before the XML's items), as in Firestorm
            S32 pos = 0;
            if (parent->getItemCount() > 0 && dynamic_cast<LLMenuItemTearOffGL*>(parent->getItem(0)))
            {
                pos = 1;
            }
            if (place.after_item[0])
            {
                pos = findMenuItemIndex(parent, place.after_item);
                if (pos < 0)
                {
                    return false;
                }
                ++pos;
            }
            else if (place.before_item && place.before_item[0])
            {
                pos = findMenuItemIndex(parent, place.before_item);
                if (pos < 0)
                {
                    return false;
                }
            }
            if (pos >= (S32)parent->getItemCount())
            {
                parent->addChild(item); // after the last item
            }
            else
            {
                parent->insert(pos, item);
            }
            return true;
        }
        return false;
    }

    // Moves the items of the menu XML file into the empty menu called target_name (a child of the menu bar,
    // or of a menu below it), or, when fs_places, to where Firestorm has them (FS_MENU_PLACES).
    void populateMenuFromFile(const char* file, const char* target_name, bool fs_places)
    {
        LLMenuGL* target = gMenuBarView->findChildMenuByName(target_name, true);
        if (!target)
        {
            LL_WARNS("Plus") << "No \"" << target_name << "\" menu in menu_viewer.xml; the items of " << file << " are not shown" << LL_ENDL;
            return;
        }

        LLMenuGL* source = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>(
            file, gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
        if (!source)
        {
            LL_WARNS("Plus") << "Cannot build " << file << LL_ENDL;
            return;
        }

        std::vector<LLMenuItemGL*> items;
        for (U32 i = 0; i < source->getItemCount(); ++i)
        {
            items.push_back(source->getItem(i));
        }

        for (LLMenuItemGL* item : items)
        {
            if (dynamic_cast<LLMenuItemTearOffGL*>(item))
            {
                continue; // the placeholder brings its own tear-off item
            }
            source->removeChild(item);
            if (!fs_places || !placeAtFsPosition(item))
            {
                target->addChild(item);
            }
        }

        if (source->getParent())
        {
            source->getParent()->removeChild(source);
        }
        delete source;

        target->needsArrange();
    }

    // The items of menu_plus.xml: the empty "Plus" menu that menu_viewer.xml declares, or the FS places.
    void populatePlusMenu()
    {
        populateMenuFromFile(PLUS_MENU_FILE, PLUS_MENU_NAME, true);
    }

    // Runs every frame until the main menu exists, then sets up the UI side once.
    bool onIdleUntilUIReady()
    {
        if (!gMenuBarView)
        {
            return false;
        }

        preloadUIImages();
        populatePlusMenu();

        for (const plus::Feature& feature : features())
        {
            if (!feature.onUIReady || failedFeatures().count(feature.name))
            {
                continue;
            }
            // As with init(): one feature that throws must not leave the idle
            // callback (the login would end there) nor skip the features after it.
            try
            {
                feature.onUIReady();
            }
            catch (const std::exception& e)
            {
                LL_WARNS("Plus") << "Feature " << feature.name << " failed in onUIReady: " << e.what() << LL_ENDL;
                failedFeatures().insert(feature.name);
            }
            catch (...)
            {
                LL_WARNS("Plus") << "Feature " << feature.name << " failed in onUIReady: unknown exception" << LL_ENDL;
                failedFeatures().insert(feature.name);
            }
        }

        return true;
    }
}

namespace plus
{

    // The graphics speed floater (where the automatic FPS adjustment was started and stopped) has no entrance
    // in Charis (hide-perf-floater). A lock kept from an earlier build would go on lowering the draw distance,
    // the reflections and the impostors every session with no way to stop it, so it is released once per
    // session. Turning AutoTuneLock off also turns AutoTuneFPS off (handleAutoTuneLockChanged).
    static void releaseAutoTune()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        if (gSavedSettings.getBOOL("AutoTuneLock"))
        {
            gSavedSettings.setBOOL("AutoTuneLock", false);
        }
        if (gSavedSettings.getBOOL("AutoTuneFPS"))
        {
            gSavedSettings.setBOOL("AutoTuneFPS", false);
        }
    }

    void onStartupStateChanged(int from, int to, const std::string& to_name)
    {
        if (to == STATE_STARTED)
        {
            releaseAutoTune();
        }
        loginfast::onStartupState(from, to, to_name);
        rlv::onStartupState(from, to);
    }

    FeatureRegistration::FeatureRegistration(Feature feature)
    {
        features().push_back(std::move(feature));
    }

    void addMenuItemsFromFile(const char* file, const char* target_name)
    {
        if (gMenuBarView)
        {
            populateMenuFromFile(file, target_name, false);
        }
    }

    void declareUIImage(const std::string& name, const std::string& file_name)
    {
        uiImages().push_back({ name, file_name });
    }

#if LL_WINDOWS
    namespace
    {
        // explorer.exe from the Windows folder by its full path (not looked up
        // on PATH or in the current folder). args is the whole command line
        // after the program name. True when the process was started.
        bool runExplorer(const std::wstring& args)
        {
            wchar_t windows_dir[MAX_PATH] = {};
            const UINT len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
            {
                LL_WARNS("Plus") << "openFolder: GetWindowsDirectoryW failed, error " << GetLastError() << LL_ENDL;
                return false;
            }
            const std::wstring exe = std::wstring(windows_dir, len) + L"\\explorer.exe";
            // CreateProcessW may write into the command line, so it gets its own buffer.
            std::wstring command = L"\"" + exe + L"\" " + args;
            std::vector<wchar_t> buffer(command.begin(), command.end());
            buffer.push_back(L'\0');

            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags     = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNORMAL;
            PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                LL_WARNS("Plus") << "openFolder: starting explorer.exe failed, error " << GetLastError() << LL_ENDL;
                return false;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }

        // The path as the shell wants it: backslashes only, no separator at the
        // end (except a drive root such as "C:\").
        std::wstring shellPath(const std::string& path)
        {
            std::wstring w = ll_convert<std::wstring>(path);
            std::replace(w.begin(), w.end(), L'/', L'\\');
            while (w.size() > 3 && w.back() == L'\\')
            {
                w.pop_back();
            }
            return w;
        }
    }
#endif

    void openFolder(const std::string& path)
    {
#if LL_WINDOWS
        if (path.empty())
        {
            return;
        }
        const std::wstring path_utf16 = shellPath(path);
        const DWORD attributes = GetFileAttributesW(path_utf16.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            LL_WARNS("Plus") << "openFolder: " << path << " does not exist (error " << GetLastError() << ")" << LL_ENDL;
            return;
        }

        // A folder goes straight to explorer.exe. ShellExecuteExW("open") on a
        // folder was seen to fail without a word (an Explorer "location is not
        // available" box the first time, nothing at all after that), and its
        // result does not tell when it hands the folder to an Explorer that then
        // fails. Starting explorer.exe with the path is what the shell does in the end.
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            runExplorer(L"\"" + path_utf16 + L"\"");
            return;
        }

        // A file opens with its program. The shell may use COM for that, so this
        // thread gets an apartment for the call if it has none (one it already
        // has, of either kind, is left as it is).
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask  = SEE_MASK_NOASYNC;
        sei.nShow  = SW_SHOWNORMAL;
        sei.lpVerb = L"open";
        sei.lpFile = path_utf16.c_str();
        const BOOL ok = ShellExecuteExW(&sei);
        const DWORD error = ok ? 0 : GetLastError();

        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }

        if (!ok)
        {
            LL_WARNS("Plus") << "openFolder: ShellExecuteExW failed for " << path << ", error " << error
                             << " (hInstApp " << (INT_PTR)sei.hInstApp << "); showing it in Explorer instead" << LL_ENDL;
            runExplorer(L"/select,\"" + path_utf16 + L"\"");
        }
#else
        LLProcess::Params params;
#if LL_DARWIN
        params.executable = "open";
#else
        params.executable = "xdg-open";
#endif
        params.args.add(path);
        params.autokill = false;
        LLProcess::create(params);
#endif
    }

    bool textHasKeyboard()
    {
        const LLUICtrl* focus = dynamic_cast<const LLUICtrl*>(gFocusMgr.getKeyboardFocus());
        return focus && focus->acceptsTextInput();
    }

#if LL_WINDOWS
    std::wstring ownExePath()
    {
        std::vector<wchar_t> path(MAX_PATH);
        for (;;)
        {
            const DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
            if (len == 0)
            {
                return std::wstring();
            }
            if (len < path.size())
            {
                return std::wstring(path.data(), len);
            }
            if (path.size() >= 32768)   // the longest path Windows has
            {
                return std::wstring();
            }
            path.resize(path.size() * 2);
        }
    }
#endif

    std::string safeText(const std::string& untrusted)
    {
        // No "<" of theirs at all (it becomes the full-width one), rather than taking out each
        // "</nolink>": a closing tag can also be made later, after this check, from what is
        // left. The text box drops every CR ("</no\rlink>"), an LLTextBox fills [ARGS] once
        // more with the default ones ("</no[sourceid]link>", sourceid being empty), and the
        // link parser's case-insensitive match follows the locale. NUL goes too: the parser
        // reads a C string and would not see our closing tag after it.
        static const std::string CLOSE      = "</nolink>";
        static const std::string FULL_LT    = "\xEF\xBC\x9C"; // U+FF1C
        static const std::string FULL_BRACE = "\xEF\xBC\xBB"; // U+FF3B
        std::string text;
        text.reserve(untrusted.size() + 16);
        for (char c : untrusted)
        {
            if (c == '<')
            {
                text += FULL_LT;
            }
            else if (c != '\r' && c != '\0')
            {
                text += c;
            }
        }

        // Line by line: the protection then does not depend on whether the link parser's
        // "." matches a line break. A "[" left open at the end of a line (no "]" after it)
        // also becomes the full-width one: with a "]" of the text around it, the second
        // filling of [ARGS] could take our closing tag into an argument ("[SECOND_LIFE,number,0").
        std::string out;
        out.reserve(text.size() + 32);
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            if (end > start)
            {
                const std::string raw   = text.substr(start, end - start);
                const size_t      close = raw.rfind(']');
                const size_t      tail  = close == std::string::npos ? 0 : close + 1;
                std::string       line  = raw.substr(0, tail);
                for (size_t i = tail; i < raw.size(); ++i)
                {
                    if (raw[i] == '[')
                    {
                        line += FULL_BRACE;
                    }
                    else
                    {
                        line += raw[i];
                    }
                }
                out += "<nolink>";
                out += line;
                out += CLOSE;
            }
            if (end == text.size())
            {
                break;
            }
            out += '\n';
            start = end + 1;
        }
        return out;
    }

    bool isWindowsDeviceName(const std::string& file_name)
    {
        std::string stem = file_name.substr(0, file_name.find('.'));
        while (!stem.empty() && stem.back() == ' ')
        {
            stem.pop_back();
        }
        for (char& c : stem)
        {
            if (c >= 'a' && c <= 'z')
            {
                c = char(c - 'a' + 'A');
            }
        }
        // COM and LPT with 0-9 and the superscripts 1-3 (Windows takes those as devices too).
        static const char* const devices[] = { "CON", "CONIN$", "CONOUT$", "PRN", "AUX", "NUL", "CLOCK$" };
        for (const char* device : devices)
        {
            if (stem == device)
            {
                return true;
            }
        }
        if (stem.size() >= 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0))
        {
            const std::string number = stem.substr(3);
            if ((number.size() == 1 && number[0] >= '0' && number[0] <= '9') ||
                number == "\xC2\xB9" || number == "\xC2\xB2" || number == "\xC2\xB3")
            {
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // No path (\ / :), none of * ? " < > | (Windows file names cannot have them) and no
        // control character (NUL included), and not a device name, with or without the spaces
        // at its start (a name read from the folder or given by a tool may start with one).
        bool isPlainFileStem(const std::string& name)
        {
            if (name.empty())
            {
                return false;
            }
            for (char c : name)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20 || u == 0x7f || std::strchr("\\/:*?\"<>|", c))
                {
                    return false;
                }
            }
            const size_t first = name.find_first_not_of(' ');
            return !isWindowsDeviceName(name) && (first == std::string::npos || !isWindowsDeviceName(name.substr(first)));
        }
    }

    bool isSafeFileStem(const std::string& name)
    {
        if (name.size() > 200 || !isPlainFileStem(name))
        {
            return false;
        }
        if (name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ')
        {
            return false;
        }
        return name.find("..") == std::string::npos;
    }

    bool isLoadableFileStem(const std::string& name)
    {
        if (name.find_first_not_of('.') == std::string::npos)
        {
            return false; // ".", "..", "...": not a name (empty is refused here too)
        }
        return isPlainFileStem(name);
    }

    bool isRegionAround(const LLViewerRegion* region)
    {
        const LLViewerRegion* agent_region = gAgent.getRegion();
        if (!region || !agent_region || region == agent_region)
        {
            return true;
        }
        // From the agent to the nearest point of the region's square, across the ground, on
        // each axis apart (the larger of the two): the eight neighbours are always within one
        // region's width on both, so they are always around, from any corner and with any draw
        // distance (straight-line distance would leave out the far diagonal one).
        const LLVector3d agent  = gAgent.getPositionGlobal();
        const LLVector3d origin = region->getOriginGlobal();
        const F64 width = region->getWidth();
        const F64 dx = llmax(origin.mdV[VX] - agent.mdV[VX], agent.mdV[VX] - (origin.mdV[VX] + width), 0.0);
        const F64 dy = llmax(origin.mdV[VY] - agent.mdV[VY], agent.mdV[VY] - (origin.mdV[VY] + width), 0.0);
        const F64 reach = (F64)gAgentCamera.mDrawDistance + (F64)agent_region->getWidth();
        return llmax(dx, dy) <= reach;
    }

    bool isNoPost()
    {
        return isNoPost(LLEnvironment::instance().getCurrentSky().get());
    }

    bool isNoPost(const LLSettingsSky* sky)
    {
        // The same test as LLPipeline::tonemap's no_post.
        static LLCachedControl<bool> build_no_post(gSavedSettings, "RenderDisablePostProcessing", false);
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        return gSnapshotNoPost || (sky && sky->getReflectionProbeAmbiance(should_auto_adjust) == 0.f) ||
               (build_no_post && gFloaterTools && gFloaterTools->isAvailable());
    }

    F32 effectiveExposure()
    {
        static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
        // NaN would pass llclamp() into every pass that adds light.
        return (isNoPost() || !llfinite((F32)exposure)) ? 1.f : llclamp((F32)exposure, 0.5f, 4.f);
    }

    F32 exposureScaleForPasses()
    {
        return isNoPost() ? 0.f : effectiveExposure();
    }

    void RetryWait::failed()
    {
        mWaiting = true;
        mNextTry = LLFrameTimer::getElapsedSeconds() + RETRY_SECONDS;
    }

    bool RetryWait::waitOver() const
    {
        return LLFrameTimer::getElapsedSeconds() >= mNextTry;
    }

    LLError::ELevel ResizeLog::level(U32 width, U32 height, U32 detail)
    {
        if (width == mWidth && height == mHeight && detail == mDetail)
        {
            return LLError::LEVEL_DEBUG;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - mLastInfo < INTERVAL_SECONDS)
        {
            return LLError::LEVEL_DEBUG;
        }
        mWidth = width;
        mHeight = height;
        mDetail = detail;
        mLastInfo = now;
        return LLError::LEVEL_INFO;
    }

    bool isClassicSky()
    {
        return isClassicSky(LLEnvironment::instance().getCurrentSky().get());
    }

    bool isClassicSky(const LLSettingsSky* sky)
    {
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        return sky && sky->canAutoAdjust() && !should_auto_adjust;
    }

    LLColor3 skyAmbient(const LLSettingsSky* sky, bool classic)
    {
        const LLColor4 ambient = sky->getTotalAmbient();
        return classic ? LLColor3(ambient.mV[VRED], ambient.mV[VGREEN], ambient.mV[VBLUE]) : linearColor3(ambient);
    }

    F32 luminance(const LLColor3& color)
    {
        return 0.2126f * color.mV[VRED] + 0.7152f * color.mV[VGREEN] + 0.0722f * color.mV[VBLUE];
    }

    void declareSettings()
    {
        static bool declared = false;
        if (declared)
        {
            return;
        }
        declared = true;

        const std::set<std::string> ours = loadSettings();
        retireOldSettings(ours);
        resetUnsavedSwitches(ours);
    }

    U32 saveSettingsFile(LLControlGroup& group, const std::string& path, bool nondefault_only)
    {
        if (path.empty())
        {
            return 0;
        }
        if (&group == &gSavedSettings && on_main_thread())
        {
            // A compare (the photo tools' button) holds the values from before the
            // changes; ending it puts the user's back (nothing when not comparing).
            plus::photomode::compareEnd();
        }
        U32 saved = 0;
        const FileWrite wrote = replaceFile(path, [&](const std::string& temp)
        {
            LLFile::remove(temp, ENOENT);   // one left by a crash must not pass for this save
            saved = group.saveToFile(temp, nondefault_only);  // its own format, as at quit
            // saveToFile() does not tell a failed write (a full disk) from a good one,
            // and returns 0 for an empty group too: the file decides.
            bool whole = false;
            {
                llifstream in(temp.c_str());
                LLSD check;
                // (Nothing to save writes an undefined value, not a map.)
                whole = in.is_open() && LLSDSerialize::fromXML(check, in) != LLSDParser::PARSE_FAILURE
                     && (saved == 0 || (check.isMap() && check.size() == (S32)saved));
            }
            if (!whole)
            {
                LL_WARNS("Plus") << "Settings not saved to " << path << ": " << temp << " could not be written whole" << LL_ENDL;
                return FileWrite::cannot_write;
            }
            return FileWrite::written;
        });
        if (wrote == FileWrite::cannot_replace)
        {
            LL_WARNS("Plus") << "Settings not saved: cannot replace " << path << LL_ENDL;
        }
        return wrote == FileWrite::written ? saved : 0;
    }

    namespace
    {
        // The viewer's own folders (the user folder with user_settings and the accounts), as one form to
        // compare: '\' only on Windows, lower case (ASCII), ending with the separator.
        std::string folderKey(std::string path)
        {
#if LL_WINDOWS
            std::replace(path.begin(), path.end(), '/', '\\');
            LLStringUtil::toLower(path);
            constexpr char SEP = '\\';
#else
            constexpr char SEP = '/';
#endif
            if (!path.empty() && path.back() != SEP)
            {
                path += SEP;
            }
            return path;
        }

        // True when path is in the viewer's own user folder (user_settings, the accounts): removeStaleTemps()
        // tidies only there, never in a folder the user picked (an export to the desktop, a NAS), where
        // other programs' files of that form may be and a whole folder may be large or slow to read. Not
        // the cache: its place is the user's choice (CacheLocation, a drive root or the home folder even),
        // and nothing is saved there through replaceFile(). A path with ".." is never taken as inside.
        bool inViewerFolders(const std::string& path)
        {
            if (!gDirUtilp || path.find("..") != std::string::npos)
            {
                return false;
            }
            const std::string key  = folderKey(path);
            const std::string root = folderKey(gDirUtilp->getOSUserAppDir());
            return root.size() > 1 && key.size() > root.size() && key.compare(0, root.size(), root) == 0;
        }

        // True the first time path's leftovers are asked for in this session: each file's folder is read
        // once, not at every save (an outfit backup writes every few seconds while scripts move the
        // attachments, into the account folder that holds the chat logs too). What a crash leaves after
        // that is tidied at the next start. Any thread.
        bool firstTidyOf(const std::string& path)
        {
            // never freed: a save made while the program ends must not find them gone
            static std::mutex* const mutex = new std::mutex;
            static std::set<std::string>* const done = new std::set<std::string>;
            const std::lock_guard<std::mutex> lock(*mutex);
            return done->insert(folderKey(path)).second;
        }
    }

    // tempPathFor() names the file after the process, so a viewer that died while saving leaves
    // "<file>.<pid>.tmp" behind for good. Only that exact name of this file goes: not this
    // process's own, and only one untouched for an hour (another viewer may be saving now).
    // Only in the viewer's own folders (inViewerFolders()), and once per file in a session
    // (firstTidyOf()). Tidying only: anything thrown here (fsyspath::string() on a name that is
    // not valid UTF-16, say) is logged and dropped, so the save that just succeeded stays a success.
    void removeStaleTemps(const std::string& path)
    {
        namespace fs = std::filesystem;
        if (!inViewerFolders(path) || !firstTidyOf(path))
        {
            return;
        }
        try
        {
            const fsyspath file(path);
            const fs::path dir = file.parent_path();
            const std::string prefix = fsyspath(file.filename()).string() + ".";
            const std::string own    = fsyspath(fsyspath(tempPathFor(path)).filename()).string();
            constexpr std::string_view suffix = ".tmp";
            if (dir.empty() || prefix.size() <= 1)
            {
                return;
            }
            const fs::file_time_type now = fs::file_time_type::clock::now();
            std::error_code ec;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::string name = fsyspath(it->path().filename()).string();
                if (name == own || name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                {
                    continue;
                }
                const std::string pid = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
                if (pid.find_first_not_of("0123456789") != std::string::npos)
                {
                    continue; // not one of tempPathFor()'s names
                }
                std::error_code e;
                if (!it->is_regular_file(e) || e)
                {
                    continue;
                }
                const fs::file_time_type written = fs::last_write_time(it->path(), e);
                if (e || now - written < std::chrono::hours(1))
                {
                    continue;
                }
                if (fs::remove(it->path(), e))
                {
                    LL_INFOS("Plus") << "Removed " << name << ", left by a save that did not finish" << LL_ENDL;
                }
            }
        }
        catch (const std::exception& ex)
        {
            LL_WARNS("Plus") << "Left-over temporary files next to " << path << " not checked: " << ex.what() << LL_ENDL;
        }
    }

    std::string tempPathFor(const std::string& path)
    {
        return path + "." + std::to_string(LLApp::getPid()) + ".tmp";
    }

    FileWrite replaceFile(const std::string& path, const std::function<FileWrite(const std::string& temp)>& write_temp,
                          int replace_attempts, bool tidy)
    {
        const std::string temp = tempPathFor(path);
        const FileWrite wrote = write_temp(temp);
        if (wrote != FileWrite::written)
        {
            if (wrote != FileWrite::cannot_open)
            {
                LLFile::remove(temp, ENOENT);   // a cut-off file must not stay beside the good one
            }
            return wrote;
        }
        for (int attempt = 1; LLFile::rename(temp, path) != 0; ++attempt)
        {
            if (attempt >= replace_attempts)
            {
                LLFile::remove(temp, ENOENT);
                return FileWrite::cannot_replace;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (tidy)
        {
            removeStaleTemps(path);   // what a crash while saving left
        }
        return FileWrite::written;
    }

    FileWrite writeFileReplacing(const std::string& path, const std::function<void(std::ostream& out)>& write,
                                 std::ios_base::openmode mode, int replace_attempts, bool tidy)
    {
        return replaceFile(path, [&](const std::string& temp)
        {
            llofstream out(temp.c_str(), mode);
            if (!out.is_open())
            {
                return FileWrite::cannot_open;
            }
            write(out);
            out.close();   // the data reaches the disk here: a failure shows only now
            return out.fail() ? FileWrite::cannot_write : FileWrite::written;
        }, replace_attempts, tidy);
    }

    std::string putAside(const std::string& path, int names, int first_number)
    {
        std::string bad = path + ".bad";
        for (int i = 0; i + 1 < names && LLFile::isfile(bad); ++i)
        {
            bad = path + ".bad" + std::to_string(first_number + i);
        }
        if (LLFile::isfile(bad))
        {
            LLFile::remove(bad);   // every name taken: the last one gives way
        }
        return LLFile::rename(path, bad) == 0 ? bad : std::string();
    }

    namespace
    {
        // The index of the '>' that ends the markup from 'from', outside quotes (and, in a DOCTYPE, outside
        // its [internal subset]); npos when the text ends first.
        size_t markupEnd(const std::string& text, size_t from)
        {
            char quote = 0;
            int brackets = 0;
            for (size_t k = from; k < text.size(); ++k)
            {
                const char c = text[k];
                if (quote)
                {
                    quote = c == quote ? 0 : quote;
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '[')
                {
                    ++brackets;
                }
                else if (c == ']')
                {
                    --brackets;
                }
                else if (c == '>' && brackets <= 0)
                {
                    return k;
                }
            }
            return std::string::npos;
        }

        // Past the end of 'close' from 'from', or npos.
        size_t pastEnd(const std::string& text, size_t from, const char* close)
        {
            const size_t at = text.find(close, from);
            return at == std::string::npos ? at : at + strlen(close);
        }
    }

    bool xmlNestingOk(const std::string& path, int max_depth)
    {
        std::string text;
        {
            llifstream file(path.c_str(), std::ios::binary);
            if (!file.is_open())
            {
                return true;
            }
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        int depth = 0;
        size_t i = 0;
        while ((i = text.find('<', i)) != std::string::npos && i + 1 < text.size())
        {
            const char next = text[i + 1];
            if (text.compare(i, 4, "<!--") == 0)
            {
                i = pastEnd(text, i + 4, "-->");
            }
            else if (text.compare(i, 9, "<![CDATA[") == 0)
            {
                i = pastEnd(text, i + 9, "]]>");
            }
            else if (next == '?')
            {
                i = pastEnd(text, i + 2, "?>");
            }
            else if (next == '!' || next == '/')
            {
                // a DOCTYPE, or an end tag
                const size_t end = markupEnd(text, i + 2);
                if (next == '/' && depth > 0)
                {
                    --depth;
                }
                i = end == std::string::npos ? end : end + 1;
            }
            else
            {
                const size_t end = markupEnd(text, i + 1);
                if (end == std::string::npos)
                {
                    break;
                }
                if (text[end - 1] != '/' && ++depth > max_depth)  // not <empty/>
                {
                    return false;
                }
                i = end + 1;
            }
            if (i == std::string::npos)
            {
                break;
            }
        }
        return true;
    }

    void registerFloaters()
    {
        static bool initialized = false;
        if (initialized)
        {
            return;
        }
        initialized = true;

        declareSettings(); // normally done already, in LLAppViewer::initConfiguration()
        LLEventPumps::instance().obtain("StartupState").listen("PlusAccountSettingsReset", &onStartupState);

        for (const Feature& feature : features())
        {
            LL_INFOS("Plus") << "Initializing feature " << feature.name << LL_ENDL;
            if (feature.init)
            {
                // One feature that fails must not take the whole start down: its
                // onUIReady is then skipped (what init registered before it threw stays).
                try
                {
                    feature.init();
                }
                catch (const std::exception& e)
                {
                    LL_WARNS("Plus") << "Feature " << feature.name << " failed to initialize: "
                                     << e.what() << LL_ENDL;
                    failedFeatures().insert(feature.name);
                }
                catch (...)
                {
                    LL_WARNS("Plus") << "Feature " << feature.name
                                     << " failed to initialize: unknown exception" << LL_ENDL;
                    failedFeatures().insert(feature.name);
                }
            }
        }

        doOnIdleRepeating(&onIdleUntilUIReady);
    }
}
