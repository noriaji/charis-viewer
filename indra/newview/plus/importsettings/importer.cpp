/**
 * @file importer.cpp
 * @brief Brings settings over from another viewer's folder (research/143).
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

// The source folder is only read: nothing in it is written, moved or deleted.
// Its saved logins are read from a copy in our own folder (the store that reads
// them writes its file back when it goes).
//
// Settings (general, graphics, account): the values of settings this viewer has
// (same name, same type, kept between sessions) are set in this session, as
// loading a settings file does; the settings of other viewers are left out.
// Never brought over (kindOf()): the cache's place (a cache shared by two viewers
// is read, written and purged by both), the version and first-start marks and the
// state of that install (a failed GL start, a pending hardware probe, its shader
// cache), the updaters', what weakens the viewer's safety (certificate checks,
// several viewers at once), the typed login data, web addresses (ours are ours),
// development, test and logging switches, ours (PlusImport*, PlusPhotoKeys*) and,
// from another family of viewers, the skin (a skin we do not have shows no UI).
// A path into the source's folders becomes the same path into ours (the chat
// log folder, InstantMessageLogPath, is one). Ours are saved first as
// settings_before_import_<date>.xml (and settings_per_account_before_import_<date>.xml);
// the last KEEP_BACKUPS of each are kept. The shaders are not built again for each
// setting that would build them (NoShaderReload), but once after all of them.
//
// An account that is logged in gets its settings in this session; another one
// in its settings_per_account.xml, which it reads at its next login.
//
// Files (account files, transcripts, poses, environments, ...): copied only
// where we have no file of that name. Ours are never replaced. The accounts'
// own files at once; the chat logs and user_settings' folders (thousands of
// files, for some) a few each frame, up to MAX_TOTAL_FILES / MAX_TOTAL_BYTES.
// Graphics presets only with the graphics: they are the source's renderer's.
// Presets go into the accounts' presets folders, the only ones this viewer reads
// (as the official viewer: LLPresetsManager::getPresetsDir); Firestorm keeps them
// in user_settings (FIRE-19810), and those go to each of its accounts' folders
// here and to the account logged in (fix-round11 1). Another Charis Viewer's
// copies of the retired Charis presets are not copied (research/183).
// The graphics preset in use (PresetGraphicActive) is kept only when that preset
// is there (activePresetChecked()).
//
// Saved logins: those we do not have are added; ours stay as they are.
//
// Key bindings: the source's key_bindings.xml replaces ours (kept as
// key_bindings_before_import_<date>.xml), then the photo tools' keys are added
// to it where it has none (plus::photomode::addDefaultKeys()).

#include "llviewerprecompiledheaders.h"

#include "importsettings.h"
#include "plus.h"
#include "photomode/photokeys.h"
#include "presets/presets.h"

#include "fsyspath.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llerrorcontrol.h"
#include "llpanellogin.h"
#include "llpresetsmanager.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsechandler_basic.h"
#include "llstartup.h"
#include "llstring.h"
#include "lltimer.h"
#include "lluicolortable.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewershadermgr.h"
#include "llrhi.h"
#include "llxmlnode.h"
#include "pipeline.h"
#include "llxuiparser.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>

namespace fs = std::filesystem;

namespace
{
    using namespace plus::importsettings;

    constexpr std::uintmax_t MAX_FILE_BYTES = 64ull << 20;
    // A settings or colours file read whole on the main thread (a real one is well under 1 MB; 64 MB held the
    // viewer about 5 s).
    constexpr std::uintmax_t MAX_SETTINGS_BYTES = 16ull << 20;
    // One bringing over copies at most this much (more is noted and left).
    constexpr std::uintmax_t MAX_TOTAL_BYTES = 2ull << 30;
    constexpr U32 MAX_TOTAL_FILES = 100000;
    // The files copied in one frame: until this long has gone (one big file may take longer).
    constexpr F64 COPY_SECONDS_PER_FRAME = 0.010;
    constexpr size_t MAX_NOTES = 60;
    constexpr size_t KEEP_BACKUPS = 5;      // of each kind of backup

    // Settings never brought over (see the top of the file). Every setting of this viewer was
    // gone through (settings.xml, settings_per_account.xml, settings_plus.xml, 2026-10-22).
    const char* const NEVER[] = {
        // the cache's place and state: a cache shared by two viewers is read, written and purged by both
        "CacheLocation", "CacheLocationTopFolder", "NewCacheLocation", "NewCacheLocationTopFolder",
        "DiskCacheDirName", "CacheValidateCounter", "DiskCacheVersion", "LocalCacheVersion",
        "MigrateCacheDirectory", "PurgeCacheOnNextStartup", "PurgeCacheOnStartup", "PurgeDiskCacheOnStartup",
        // this install's version and first-start marks
        "LastRunVersion", "LastFeatureVersion", "LastUIFeatureVersion", "LastGPUClass", "LastGPUString",
        "FirstRunThisInstall", "FirstLoginThisInstall", "VersionChannelName", "CrashSubmitBehavior",
        "SkipBenchmark", "PlusReleaseQAModeCleared",
        // the state of that install (2026-10-22, fix-round9a): a GL start that failed there
        // (RenderInitError: graphics at 0 and a warning at our next start), a hardware probe it
        // still had to make (ProbeHardwareOnStartup: the recommended settings over the ones
        // brought), its shader cache, its installer's language, what it found or told once.
        // JoystickInitialized is brought (2026-10-22, fix-round10): it goes with the 3D mouse's
        // settings (Joystick*), which are brought. It says the defaults of that device were put
        // in already; without it, the next start with a SpaceNavigator puts them in over the
        // ones brought and switches the 3D mouse on. The other marks here go with nothing that
        // is brought: LastGPUString / LastFeatureVersion / ProbeHardwareOnStartup keep ours, so
        // the recommended settings are not put in over the graphics brought; the cache's marks
        // go with the cache's place and the shader cache's with its files, none of them brought.
        "RenderInitError", "ProbeHardwareOnStartup", "RenderShaderCacheVersion", "AlertedUnsupportedHardware",
        "PreviousInstallChecked", "InstallLanguage", "NumSessions",
        // fix-round68 4A: set for this run only (an unsaved value) at every start; a value set here
        // would take that away (setValue() returns to the default first) and come into effect.
        // RenderShaderCacheEnabled: off under Vulkan (vkcheck.cpp, the GL program binaries have no
        // sources to translate). SystemLanguage: what Windows says (llappviewerwin32.cpp).
        "RenderShaderCacheEnabled", "SystemLanguage",
        // the losses of that install's GPU with DGC in use (fix-round10 3): its own GPU and driver
        "PlusGpuDrivenDGCLostCount",
        // safety: the certificate checks, the embedded browser's local files, several viewers at once
        "NoVerifySSLCert", "BrowserIgnoreSSLCertErrors", "CertStore", "BrowserFileAccessFromFileUrls",
        "AllowMultipleViewers",
        // a program the viewer starts (the external script editor's command line, security audit A L5):
        // set in this viewer, not brought from a file another program could have written
        "ExternalEditor",
        // the login: typed login data (a password can be in it), the grid, the login pages
        "UserLoginInfo", "CurrentGrid", "ServerChoice", "LoginPage", "GridStatusRSS",
        // development and test switches
        "QuitOnLoginActivated", "SpeedTest", "TextureDisable", "TextureDecodeDisabled", "RunMultipleThreads",
        "UIFloaterTestBool", "TestGridStatusRSSFromFile", "FMODExProfilerEnable", "PerfStatsCaptureEnabled",
        "RenderPerformanceTest", "MediaPluginForceVLC", "PlusHairSwayDetectOnly", "PlusGazeAllowEveryone",
        // logs and their files
        "UserLogFile", "LogMessages", "LogTextureNetworkTraffic", "LogTextureDownloadsToViewerLog",
        "LogTextureDownloadsToSimulator", "LogWearableAssetSave", "MeshUploadLogXML", "MemoryLogFrequency",
        "TextureFetchMinTimeToLog", "TextureLoggingThreshold", "CefVerboseLog", "PlusUiFrameLog",
        "PlusSelfFirstLog", "PlusAOLogTiming", "PlusPathingDumpBlob",
    };
    const char* const NEVER_PREFIXES[] = {
        "Updater", "PlusUpdateCheck",       // the updaters (and the hosts ours may download from)
        "PlusImport", "PlusPhotoKeys",      // ours; the photo tools' keys follow the key bindings
        "PlusPoserMcp",                     // a server for other programs: switched on in this viewer, not brought
        "QAMode", "Stats", "Force",         // test modes, statistics runs, forced failures and addresses
        "RestrainedLove", "RLVa",           // RLVa (plus/rlv): remote scripted viewer controls are switched on in this viewer, never by a file brought over
    };
    // Anywhere in the name: development switches (DebugShow*, ...Debug), diagnostics, forced
    // failures, logging.
    const char* const NEVER_PARTS[] = { "Debug", "Diag", "Fake", "Simulate", "Logging" };
    // Text settings whose name has one of these hold a web address: ours are ours.
    const char* const ADDRESS_PARTS[] = { "URL", "Url", "URI", "Uri" };
    // Only from another Charis Viewer.
    const char* const SKIN[] = { "SkinCurrent", "SkinCurrentTheme" };
    // Graphics: what the picture looks like, and the window. The Charis Viewer's own picture
    // settings (only another Charis Viewer has them) are graphics too.
    const char* const GRAPHICS_PREFIXES[] = {
        "Render", "Window", "FullScreen",
        "PlusGtao", "PlusContactShadow", "PlusShadow", "PlusSkin", "PlusWet", "PlusRain", "PlusVolumetric", "PlusGroundFog", "PlusFirefl",
        "PlusDoFAlpha", "PlusGraphicPreset", "PlusSnapshotBoost", "PlusGlare", "PlusLensFlare", "PlusCrossFilter",
        "PlusLook", "PlusPaint", "PlusFx", "PlusMiniature", "PlusOcean", "PlusMirrorAvatars", "PlusAvatarReflection",
        "PlusWaterMirror", "PlusSSR", "PlusHairSway", "PlusGpuDrivenDGC", "PlusGPUBenchmark",
    };
    const char* const GRAPHICS[] = { "VertexShaderEnable", "WindLightUseAtmosShaders", "PresetGraphicActive",
                                     "FramePerSecondLimit", "UseOcclusion", "TextureMemory" };
    // Of those prefixes, the user's own lists (what hair to sway, favorite looks): general.
    // (The accounts' settings are brought over as general only: PlusHairSwayManual is one.)
    const char* const NOT_GRAPHICS[] = { "PlusHairSwayManual", "PlusLookFavorites", "PlusLookRecent" };
    // The presets' folder (in an account's folder; Firestorm's in user_settings) and, in it,
    // the graphics presets' folder.
    constexpr char PRESETS[] = "presets";
    constexpr char GRAPHIC_PRESETS[] = "graphic";
    // The graphics preset in use (global, though the presets are the account's).
    constexpr char ACTIVE_PRESET_SETTING[] = "PresetGraphicActive";
    // The official default graphics preset: made in the account's folder when missing
    // (LLPresetsManager::createMissingDefault).
    constexpr char DEFAULT_PRESET[] = "Default";
    // In plus/presets' copies of the Charis presets only (the official "Save preset"
    // never writes it).
    constexpr char CHARIS_PRESET_MARK[] = "PlusGraphicPresetRevision";
    // plus/presets names its copies so: another language's copy ("Charis 標準") is
    // replaced by this viewer's at the login (recheckActivePreset there).
    constexpr char CHARIS_PRESET_PREFIX[] = "Charis ";

    // Folders of the viewer's folder that are not accounts.
    const char* const NOT_ACCOUNTS[] = { "user_settings", "logs", "downloads", "browser_profile", "cef_cache",
                                         "cef_cookies", "cache", "userdata" };
    // Folders not copied at any depth: browser data and caches, made again by the viewer.
    const char* const SKIPPED_DIRS[] = { "cache", "cef_cache", "cef_cookies", "browser_profile" };
    // Files not copied: logs, run markers, locks, dumps, half-written files.
    const char* const SKIPPED_FILE_ENDINGS[] = { ".log", ".old", ".lock", ".dmp", "_marker", ".tmp" };
    // Files of an account that are not settings: the login screen's pictures, lists the viewer
    // makes again, notifications and offers of past sessions, our own backups.
    // plus_outfit_backup (fix-round52 2): plus/outfitbackup's copy of what was worn
    // ("plus_outfit_backup.xml", ".<grid>.xml" off the main grid). Another install's would
    // offer to put back an outfit from its own sessions, not this viewer's.
    const char* const SKIPPED_ACCOUNT_PREFIXES[] = { "screen_", "texture_list_", "open_notifications_",
                                                     "settings_per_account_before_import", "plus_outfit_backup" };
    const char* const SKIPPED_ACCOUNT_FILES[] = { "inbox_new_items.xml", "experience_events.xml",
                                                  "charis_graphics_before_preset.xml" };

    // Saved-login data whose entries are one per user (merged entry by entry); any other kind
    // ("credential": the last login of a grid) is taken whole when we have none for that grid.
    // The official viewer and Firestorm key them alike: the grid ("util.agni.lindenlab.com"),
    // then "first_last" in lower case (LLSecAPIBasicCredential::userID()).
    const char* const PER_USER_LOGIN_DATA[] = { "login_list", "mfa_hash" };

    // Backups of ours: <name>_<date and time>.xml
    constexpr char BACKUP_SETTINGS[] = "settings_before_import";
    constexpr char BACKUP_ACCOUNT_SETTINGS[] = "settings_per_account_before_import";
    constexpr char BACKUP_KEYS[] = "key_bindings_before_import";
    constexpr char BACKUP_WARNINGS[] = "ignorable_dialogs_before_import";  // fix-round68 2
    constexpr char BACKUP_COLORS[] = "colors_before_import";               // fix-round68 2
    constexpr char LOGINS_COPY[] = "bin_conf_import.tmp";

    constexpr char CHAT_LOG_SETTING[] = "InstantMessageLogPath";    // per account
    constexpr char ACCOUNT_SETTINGS_FILE[] = "settings_per_account.xml";

    enum class Kind { GENERAL, GRAPHICS, NEVER };

    // The source's folder -> ours. bare: the source's folder is a root alone ("D:\", "\\host\"),
    // so only a value naming that root itself is rewritten, not the paths inside it (they
    // are the rest of the drive, not the viewer's folder).
    struct Root
    {
        std::string from;
        std::string to;
        bool bare = false;
    };
    using Roots = std::vector<Root>;

    // A file to copy later (a few each frame).
    struct Copy
    {
        fs::path from;
        fs::path to;
    };

    // Notes for the log, at most MAX_NOTES of them.
    void addNote(Result& result, const std::string& line)
    {
        if (result.notes.size() < MAX_NOTES)
        {
            result.notes.push_back(line);
        }
        else if (result.notes.size() == MAX_NOTES)
        {
            result.notes.push_back("(more not listed)");
        }
    }

    struct Context
    {
        const Source& source;
        const Choice& choice;
        Result& result;
        fs::path from;      // the source's folder
        fs::path to;        // ours
        Roots roots;
        std::vector<Copy> copies;           // the files to copy later
        std::uintmax_t copies_bytes = 0;
        bool total_limit_noted = false;
        // PresetGraphicActive before the settings went in (none: the graphics were not brought).
        std::optional<std::string> active_preset_before;
        // The source's settings.xml has PresetGraphicActive (a settings file leaves out the
        // values at their default: none means no preset was in use there).
        bool source_names_active_preset = false;
        bool graphics_changed = false;      // a graphics setting of ours changed
        // The name kept because that preset is being copied (or was there before the login):
        // looked at again when the copies are done (finishJob()).
        std::string active_preset_kept;

        void note(const std::string& line)
        {
            addNote(result, line);
        }
    };

    std::string utf8(const fs::path& path)
    {
        return fsyspath(path).string();
    }

    std::string lowered(std::string text)
    {
        LLStringUtil::toLower(text);
        return text;
    }

    template<size_t N>
    bool isOneOf(const std::string& text, const char* const (&list)[N])
    {
        for (const char* item : list)
        {
            if (text == item)
            {
                return true;
            }
        }
        return false;
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
        return lowered(key);
    }

    bool isSeparator(char c)
    {
        return c == '\\' || c == '/';
    }

    // Length of a folder path without the separators at its end ("D:\" -> 2, "D:\a\" -> 4).
    size_t lengthWithoutEndSeparators(const std::string& path)
    {
        size_t length = path.size();
        while (length > 0 && isSeparator(path[length - 1]))
        {
            --length;
        }
        return length;
    }

    // Case-insensitive "value is root or inside root", either slash. The root may end
    // with a separator (a drive root such as "D:\").
    bool isUnder(const std::string& value, const std::string& root)
    {
        const size_t length = lengthWithoutEndSeparators(root);
        if (length == 0 || value.size() < length)
        {
            return false;
        }
        if (folderKey(value.substr(0, length)) != folderKey(root.substr(0, length)))
        {
            return false;
        }
        return value.size() == length || isSeparator(value[length]);
    }

    // A root alone: a root name and/or root directory and nothing after ("D:\", "D:", "\\host\",
    // "\"). "\\host\share" and "\\host\share\" are not: their share is a folder with a name.
    bool isBareRoot(const fs::path& path)
    {
        return (path.has_root_name() || path.has_root_directory()) && path.relative_path().empty();
    }

    // The folder's own name, also when the path ends with a separator ("D:\a\" -> "a");
    // empty for a bare root.
    fs::path folderName(const fs::path& path)
    {
        if (isBareRoot(path))
        {
            return fs::path();
        }
        return path.has_filename() ? path.filename() : path.parent_path().filename();
    }

    void addRoot(Roots& roots, const fs::path& from, const std::string& to)
    {
        roots.push_back(Root{ utf8(from), to, isBareRoot(from) });
    }

    std::string moved(const std::string& value, const Roots& roots)
    {
        for (const auto& [from, to, bare] : roots)
        {
            if (bare)
            {
                if (folderKey(value) == folderKey(from))
                {
                    return to;
                }
                continue;
            }
            if (isUnder(value, from))
            {
                // The rest starts with a separator (or is empty), so neither root's own
                // end separator is kept: "D:\" + "\x" would give "D:\\x".
                const std::string rest = value.substr(lengthWithoutEndSeparators(from));
                return rest.empty() ? to : to.substr(0, lengthWithoutEndSeparators(to)) + rest;
            }
        }
        return value;
    }

    template<size_t N>
    bool hasPart(const std::string& text, const char* const (&parts)[N])
    {
        for (const char* part : parts)
        {
            if (text.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    Kind kindOf(const std::string& name, Family family, LLControlVariable& control)
    {
        if (isOneOf(name, NEVER) || (family != Family::CHARIS && isOneOf(name, SKIN)) || hasPart(name, NEVER_PARTS)
            || (control.type() == TYPE_STRING && hasPart(name, ADDRESS_PARTS)))
        {
            return Kind::NEVER;
        }
        for (const char* prefix : NEVER_PREFIXES)
        {
            if (LLStringUtil::startsWith(name, prefix))
            {
                return Kind::NEVER;
            }
        }
        if (isOneOf(name, NOT_GRAPHICS))
        {
            return Kind::GENERAL;
        }
        if (isOneOf(name, GRAPHICS))
        {
            return Kind::GRAPHICS;
        }
        for (const char* prefix : GRAPHICS_PREFIXES)
        {
            if (LLStringUtil::startsWith(name, prefix))
            {
                return Kind::GRAPHICS;
            }
        }
        return Kind::GENERAL;
    }

    // Not read whole into memory past MAX_SETTINGS_BYTES (fix-round52 4): a settings file that big is not
    // one. Nor one nested deeper than a settings file is (fix-round101: the official readers recurse when the
    // result is used or freed, and a deep file stopped the viewer).
    bool tooBigToRead(const fs::path& file)
    {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(file, ec);
        if (!ec && size > MAX_SETTINGS_BYTES)
        {
            return true;
        }
        if (!plus::xmlNestingOk(utf8(file)))
        {
            LL_WARNS("Plus") << "Import: " << utf8(file) << " is nested too deep; not read" << LL_ENDL;
            return true;
        }
        return false;
    }

    // A settings file (settings.xml format). False when it is missing, too big or not one.
    bool readSettings(const fs::path& file, LLSD& settings)
    {
        if (tooBigToRead(file))
        {
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        return in && LLSDSerialize::fromXML(settings, in) != LLSDParser::PARSE_FAILURE && settings.isMap();
    }

    // Written beside the file, then moved over it: never a cut-off settings file.
    bool writeSettings(const fs::path& file, const LLSD& settings)
    {
        return plus::writeFileReplacing(utf8(file), [&](std::ostream& out) { LLSDSerialize::toPrettyXML(settings, out); },
                                        std::ios::binary | std::ios::trunc) == plus::FileWrite::written;
    }

    // After a new backup was written (pruneBackups): the backups of that name, the last
    // KEEP_BACKUPS of them (with the new one) stay; a second bringing over does not take
    // away the first one's (the settings as they were before any).
    // fix-round52 1: only once the new one is there. Taken away before it was written, a
    // backup that then failed (a full disk) left one fewer and nothing new.
    // fix-round53 1: the new one (just_made) is never among those taken away. With the clock
    // set back, older backups have names of later dates and would sort after it.
    void pruneBackups(const fs::path& folder, const std::string& name, const fs::path& just_made)
    {
        const std::string start = name + "_";
        const fs::path keep = just_made.filename();
        std::vector<fs::path> backups;
        std::error_code ec;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            const std::string file = utf8(it->path().filename());
            std::error_code fec;
            if (LLStringUtil::startsWith(file, start) && LLStringUtil::endsWith(file, ".xml") && it->path().filename() != keep
                && it->is_regular_file(fec))
            {
                backups.push_back(it->path());
            }
        }
        // The dates sort as the names do (year first). KEEP_BACKUPS with the new one.
        std::sort(backups.begin(), backups.end());
        for (size_t i = 0; i + KEEP_BACKUPS - 1 < backups.size(); ++i)
        {
            fs::remove(backups[i], ec);
        }
    }

    // Where a new backup of ours goes: <name>_<date and time>.xml in the folder (a name not
    // in use). The older ones are left; pruneBackups() after the new one was written.
    // With all the names of this second in use, one of them: the callers write only where
    // there is no file (and fail there), never over it.
    fs::path backupPath(const fs::path& folder, const std::string& name)
    {
        const std::string start = name + "_";
        std::error_code ec;
        std::time_t now = std::time(nullptr);
        std::tm local{};
#if LL_WINDOWS
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char date[32];
        std::strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &local);
        fs::path path = folder / (start + date + ".xml");
        // Twice in a second: a number after it ("_2" sorts after ".xml", as it is newer).
        for (int n = 2; fs::exists(path, ec) && n < 10; ++n)
        {
            path = folder / (start + date + "_" + std::to_string(n) + ".xml");
        }
        return path;
    }

    // fix-round52 1: the settings of a group in this session, backed up before they are
    // changed. False (counted and noted) when the backup could not be written: then nothing
    // is changed. saveSettingsFile() returns 0 both for a failed write and for a group with
    // nothing to save, so the file decides (it writes a new file, or none, under a name not
    // in use before).
    bool backUpGroup(LLControlGroup& group, const fs::path& folder, const std::string& name, Context& c)
    {
        const fs::path backup = backupPath(folder, name);
        std::error_code ec;
        const bool taken = fs::exists(backup, ec) || ec;
        if (taken || (plus::saveSettingsFile(group, utf8(backup), true) == 0 && !fs::is_regular_file(backup, ec)))
        {
            ++c.result.errors;
            c.note("cannot back up the settings to " + utf8(backup) + " (left as they were)");
            return false;
        }
        pruneBackups(folder, name, backup);
        return true;
    }

    // The value to set from one entry of a settings file, or none when it is not brought over:
    // a setting this viewer does not have, has with another type or does not keep, or one of a
    // kind not chosen.
    std::optional<LLSD> wanted(LLControlGroup& group, const std::string& name, const LLSD& entry,
                               bool general, bool graphics, const Context& c)
    {
        if (!entry.isMap() || !entry.has("Value"))
        {
            return std::nullopt;
        }
        LLControlVariable* control = group.getControl(name);
        if (!control || !control->isPersisted()
            || !control->isType(LLControlGroup::typeStringToEnum(entry["Type"].asString())))
        {
            return std::nullopt;
        }
        const Kind kind = kindOf(name, c.source.family, *control);
        if (kind == Kind::NEVER || (kind == Kind::GENERAL && !general) || (kind == Kind::GRAPHICS && !graphics))
        {
            return std::nullopt;
        }

        LLSD value = entry["Value"];
        if (control->type() == TYPE_BOOLEAN)
        {
            value = value.asBoolean();      // the file has 0 and 1
        }
        else if (control->type() == TYPE_STRING)
        {
            const std::string text = value.asString();
            const std::string text_moved = moved(text, c.roots);
            if (text_moved != text)
            {
                value = text_moved;
            }
        }
        return value;
    }

    // An account of ours whose chat logs are set apart from our folder (InstantMessageLogPath
    // elsewhere) keeps them there: the source's place would hide them from this viewer.
    bool keepsOwnChatLogs(const std::string& our_value, const Context& c)
    {
        return !our_value.empty() && !isUnder(our_value, utf8(c.to));
    }

    // General settings whose handler builds the shaders again (handleSetShaderChanged); the
    // others are graphics (Render*).
    const char* const SHADER_GENERAL[] = { "TerrainPaintBitDepth" };

    // Sets the values of a settings file in this session (they are saved with the group).
    // shaders_changed (if given): set when a graphics setting, or one of SHADER_GENERAL, changed.
    U32 applyToGroup(LLControlGroup& group, const fs::path& file, bool general, bool graphics, Context& c,
                     bool* shaders_changed = nullptr)
    {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
        {
            return 0;
        }
        LLSD settings;
        if (!readSettings(file, settings))
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(file));
            return 0;
        }

        U32 changed = 0;
        for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
        {
            if (&group == &gSavedSettings && it->first == ACTIVE_PRESET_SETTING)
            {
                c.source_names_active_preset = true;
            }
            const std::optional<LLSD> value = wanted(group, it->first, it->second, general, graphics, c);
            if (!value)
            {
                continue;
            }
            LLControlVariable* control = group.getControl(it->first);
            if (it->first == CHAT_LOG_SETTING && keepsOwnChatLogs(control->getValue().asString(), c))
            {
                continue;
            }
            const LLSD before = control->getValue();
            control->setValue(*value);
            if (!llsd_equals(before, control->getValue()))
            {
                ++changed;
                if (&group == &gSavedSettings && kindOf(it->first, c.source.family, *control) == Kind::GRAPHICS)
                {
                    c.graphics_changed = true;
                }
                if (shaders_changed && (kindOf(it->first, c.source.family, *control) == Kind::GRAPHICS
                                        || std::find(std::begin(SHADER_GENERAL), std::end(SHADER_GENERAL), it->first)
                                               != std::end(SHADER_GENERAL)))
                {
                    *shaders_changed = true;
                }
            }
        }
        return changed;
    }

    // An account that is not logged in: the values go into its settings_per_account.xml.
    U32 applyToAccountFile(const fs::path& from_file, const fs::path& to_file, Context& c)
    {
        std::error_code ec;
        if (!fs::is_regular_file(from_file, ec))
        {
            return 0;
        }
        LLSD settings;
        if (!readSettings(from_file, settings))
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(from_file));
            return 0;
        }
        LLSD ours = LLSD::emptyMap();
        if (fs::exists(to_file, ec) && !readSettings(to_file, ours))
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(to_file) + " (left as it was)");
            return 0;
        }

        U32 changed = 0;
        for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
        {
            const std::optional<LLSD> value = wanted(gSavedPerAccountSettings, it->first, it->second, true, false, c);
            if (!value || (ours.has(it->first) && llsd_equals(ours[it->first]["Value"], *value))
                || (it->first == CHAT_LOG_SETTING && ours.has(it->first)
                    && keepsOwnChatLogs(ours[it->first]["Value"].asString(), c)))
            {
                continue;
            }
            LLControlVariable* control = gSavedPerAccountSettings.getControl(it->first);
            LLSD entry;
            entry["Comment"] = control->getComment();
            entry["Persist"] = 1;
            entry["Type"] = LLControlGroup::typeEnumToString(control->type());
            entry["Value"] = *value;
            ours[it->first] = entry;
            ++changed;
        }
        if (changed == 0)
        {
            return 0;
        }

        fs::create_directories(to_file.parent_path(), ec);
        if (fs::exists(to_file, ec))
        {
            // fix-round52 1: no backup, no change.
            const fs::path backup = backupPath(to_file.parent_path(), BACKUP_ACCOUNT_SETTINGS);
            if (!fs::copy_file(to_file, backup, fs::copy_options::none, ec))
            {
                ++c.result.errors;
                c.note("cannot back up " + utf8(to_file) + " to " + utf8(backup) + ": " + ec.message()
                       + " (left as it was)");
                return 0;
            }
            pruneBackups(to_file.parent_path(), BACKUP_ACCOUNT_SETTINGS, backup);
        }
        if (!writeSettings(to_file, ours))
        {
            ++c.result.errors;
            c.note("cannot write " + utf8(to_file));
            return 0;
        }
        return changed;
    }

    // ---- files ---------------------------------------------------------------

    // Copies a file where we have none; false (and it is counted and noted) when it could not.
    // Both at once and later (the files copied a few each frame).
    void copyFile(const fs::path& from, const fs::path& to, Result& result)
    {
        std::error_code ec;
        if (fs::exists(to, ec) || ec)
        {
            ++result.kept;      // made meanwhile (by this viewer, at login) or there already
            return;
        }
        fs::create_directories(to.parent_path(), ec);
        // Copied beside it under another name first and then renamed: a copy cut short (the
        // viewer quitting or crashing, a full disk) never leaves a part of the file under its
        // own name, which would then be "there already" for good. A leftover of such a copy is
        // overwritten.
        fs::path part = to;
        part += ".importing";
        // A leftover may be read-only (cut short between the copy and the permissions below),
        // and a read-only file cannot be overwritten: made writable and removed first.
        if (fs::exists(part, ec))
        {
            fs::permissions(part, fs::perms::owner_write, fs::perm_options::add, ec);
            fs::remove(part, ec);
        }
        if (!fs::copy_file(from, part, fs::copy_options::overwrite_existing, ec))
        {
            const std::string why = ec.message();
            fs::remove(part, ec);
            ++result.errors;
            addNote(result, "cannot copy " + utf8(from) + ": " + why);
            return;
        }
        // A read-only copy could never be saved by this viewer.
        fs::permissions(part, fs::perms::owner_write, fs::perm_options::add, ec);
        if (fs::exists(to, ec))
        {
            fs::remove(part, ec);
            ++result.kept;      // made while it was copied
            return;
        }
        fs::rename(part, to, ec);
        if (ec)
        {
            const std::string why = ec.message();
            fs::remove(part, ec);
            ++result.errors;
            addNote(result, "cannot copy " + utf8(from) + ": " + why);
            return;
        }
        ++result.files;
    }

    // Too big alone, or past what one bringing over copies: noted, not copied.
    bool overLimit(const fs::path& from, std::uintmax_t size, Context& c)
    {
        if (size > MAX_FILE_BYTES)
        {
            ++c.result.over_limit;
            c.note("not copied (" + std::to_string(size >> 20) + " MB): " + utf8(from));
            return true;
        }
        if (c.copies.size() >= MAX_TOTAL_FILES || c.copies_bytes + size > MAX_TOTAL_BYTES)
        {
            ++c.result.over_limit;
            if (!c.total_limit_noted)
            {
                c.total_limit_noted = true;
                c.note("the limit of one bringing over (" + std::to_string(MAX_TOTAL_FILES) + " files, "
                       + std::to_string(MAX_TOTAL_BYTES >> 20) + " MB) reached; not copied from: " + utf8(from));
            }
            return true;
        }
        return false;
    }

    // At once: the accounts' own files (read at login, which may start right after).
    void copyIfMissing(const fs::path& from, const fs::path& to, Context& c)
    {
        std::error_code ec;
        if (fs::exists(to, ec) || ec)
        {
            ++c.result.kept;
            return;
        }
        const std::uintmax_t size = fs::file_size(from, ec);
        if (ec)
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(from) + ": " + ec.message());
            return;
        }
        if (overLimit(from, size, c))
        {
            return;
        }
        copyFile(from, to, c.result);
    }

    // Later, a few each frame (copyFile() checks again that we still have none).
    void queueIfMissing(const fs::directory_entry& from, const fs::path& to, Context& c)
    {
        std::error_code ec;
        if (fs::exists(to, ec) || ec)
        {
            ++c.result.kept;
            return;
        }
        const std::uintmax_t size = from.file_size(ec);     // from the folder's listing
        if (ec)
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(from.path()) + ": " + ec.message());
            return;
        }
        if (overLimit(from.path(), size, c))
        {
            return;
        }
        c.copies.push_back({ from.path(), to });
        c.copies_bytes += size;
    }

    void copyTreeIfMissing(const fs::path& from, const fs::path& to, Context& c)
    {
        std::error_code ec;
        for (fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            const fs::file_status status = it->symlink_status(fec);    // links are not followed
            const std::string name = lowered(utf8(it->path().filename()));
            if (fec)
            {
                continue;
            }
            if (fs::is_directory(status))
            {
                if (!isOneOf(name, SKIPPED_DIRS))
                {
                    copyTreeIfMissing(it->path(), to / it->path().filename(), c);
                }
            }
            else if (fs::is_regular_file(status))
            {
                bool skipped = false;
                for (const char* ending : SKIPPED_FILE_ENDINGS)
                {
                    skipped = skipped || LLStringUtil::endsWith(name, ending);
                }
                if (!skipped)
                {
                    queueIfMissing(*it, to / it->path().filename(), c);
                }
            }
        }
        if (ec)
        {
            ++c.result.errors;
            c.note("cannot list " + utf8(from) + ": " + ec.message());
        }
    }

    // Another Charis Viewer's copy of a retired Charis graphics preset (plus/presets,
    // research/183): not brought over (this viewer removes them at the login).
    bool isCharisPresetCopy(const fs::path& file)
    {
        LLSD content;
        return readSettings(file, content) && content.has(CHARIS_PRESET_MARK);
    }

    // The graphics presets: the files, but the Charis presets' copies.
    void copyGraphicPresetsIfMissing(const fs::path& from, const fs::path& to, Context& c)
    {
        std::error_code ec;
        for (fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            const fs::file_status status = it->symlink_status(fec);
            const std::string name = lowered(utf8(it->path().filename()));
            if (fec || !fs::is_regular_file(status) || !LLStringUtil::endsWith(name, ".xml") || isCharisPresetCopy(it->path()))
            {
                continue;   // the viewer lists *.xml only (LLPresetsManager::loadPresetNamesFromDir)
            }
            queueIfMissing(*it, to / it->path().filename(), c);
        }
        if (ec)
        {
            ++c.result.errors;
            c.note("cannot list " + utf8(from) + ": " + ec.message());
        }
    }

    // A presets folder into an account's presets folder: the graphics presets only with the
    // graphics (they are made for the source's renderer, and choosing one applies its Render*
    // settings); the others (camera, ...) always.
    void copyPresetsIfMissing(const fs::path& from, const fs::path& to, Context& c)
    {
        std::error_code ec;
        for (fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            const fs::file_status status = it->symlink_status(fec);
            const std::string name = lowered(utf8(it->path().filename()));
            if (fec || !fs::is_directory(status))
            {
                continue;
            }
            if (name == GRAPHIC_PRESETS)
            {
                if (c.choice.graphics)
                {
                    copyGraphicPresetsIfMissing(it->path(), to / it->path().filename(), c);
                }
                continue;
            }
            copyTreeIfMissing(it->path(), to / it->path().filename(), c);
        }
    }

    // The account folders of a viewer's folder (or of a chat log folder): any folder but the
    // viewer's own ones.
    std::vector<fs::path> accountFolders(const fs::path& root)
    {
        std::vector<fs::path> folders;
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            const std::string name = lowered(utf8(it->path().filename()));
            if (!fs::is_directory(it->symlink_status(fec)) || isOneOf(name, NOT_ACCOUNTS)
                || LLStringUtil::startsWith(name, "dump-") || name.find(".migrating.") != std::string::npos)
            {
                continue;
            }
            folders.push_back(it->path());
        }
        return folders;
    }

    bool isTranscript(const std::string& name)
    {
        return LLStringUtil::endsWith(name, ".txt") || name == "conversation.log"
            || (LLStringUtil::startsWith(name, "chat_service") && LLStringUtil::endsWith(name, ".csv"));
    }

    bool isAccountFile(const std::string& name)
    {
        if (isTranscript(name) || isOneOf(name, SKIPPED_ACCOUNT_FILES))
        {
            return false;
        }
        for (const char* prefix : SKIPPED_ACCOUNT_PREFIXES)
        {
            if (LLStringUtil::startsWith(name, prefix))
            {
                return false;
            }
        }
        for (const char* ending : SKIPPED_FILE_ENDINGS)
        {
            if (LLStringUtil::endsWith(name, ending))
            {
                return false;
            }
        }
        return true;
    }

    // ---- the parts -------------------------------------------------------------

    void setShaders()
    {
        LLViewerShaderMgr::instance()->setShaders();
    }

    // Each setting whose handler builds the shaders again (handleSetShaderChanged: RenderShadowDetail,
    // RenderGlow, TerrainPaintBitDepth, ...) would build them all, once for each such setting, the
    // viewer standing still meanwhile. As presets.cpp's loadSettings(): none while the values go in,
    // one build at the end (on several threads when the Vulkan backend can).
    struct NoShaderReload
    {
        const bool was = LLViewerShaderMgr::sSkipReload;
        NoShaderReload() { LLViewerShaderMgr::sSkipReload = true; }
        ~NoShaderReload() { LLViewerShaderMgr::sSkipReload = was; }
    };

    // plus/shaderreload makes it in the background after login (research/250), else on several threads.
    void reloadShaders()
    {
        setShaders();
        if (gPipeline.isInit())
        {
            LLPipeline::refreshCachedSettings();
        }
    }

    void importGeneral(Context& c)
    {
        const fs::path settings = c.from / "user_settings" / "settings.xml";
        const std::string ours = gSavedSettings.getString("ClientSettingsFile");
        // fix-round53 1: no settings.xml of theirs, nothing of ours changed: no backup (one
        // more would push the oldest, the settings before any bringing over, out).
        std::error_code ec;
        if (fs::is_regular_file(settings, ec)
            && !backUpGroup(gSavedSettings, c.to / "user_settings", BACKUP_SETTINGS, c))
        {
            return;     // fix-round52 1: not changed without a backup
        }

        U32 changed = 0;
        bool shaders_changed = false;
        if (c.choice.graphics)
        {
            c.active_preset_before = gSavedSettings.getString(ACTIVE_PRESET_SETTING);
        }
        try
        {
            NoShaderReload no_reload;
            changed = applyToGroup(gSavedSettings, settings, c.choice.general, c.choice.graphics, c, &shaders_changed);
        }
        catch (...)
        {
            if (shaders_changed)
            {
                reloadShaders();    // for the values set before it stopped
            }
            throw;
        }
        if (shaders_changed)
        {
            reloadShaders();
        }
        c.result.settings += changed;
        if (c.choice.general)
        {
            // the warnings and questions not to show again; fix-round68 2: backed up as the
            // settings are (the confirmation says what was there is kept), not changed without
            const fs::path warnings = c.from / "user_settings" / "ignorable_dialogs.xml";
            if (fs::is_regular_file(warnings, ec)
                && backUpGroup(gWarningSettings, c.to / "user_settings", BACKUP_WARNINGS, c))
            {
                c.result.settings += applyToGroup(gWarningSettings, warnings, true, true, c);
            }
        }
        if (!ours.empty())
        {
            plus::saveSettingsFile(gSavedSettings, ours, true);
        }
    }

    // The user's own colors (colors.xml holds only those): the ones this viewer has.
    void importColors(Context& c)
    {
        const fs::path file = c.from / "user_settings" / "colors.xml";
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
        {
            return;
        }
        LLXMLNodePtr root;
        if (tooBigToRead(file) || !LLXMLNode::parseFile(utf8(file), root, nullptr) || !root || !root->hasName("colors"))
        {
            ++c.result.errors;
            c.note("cannot read " + utf8(file));
            return;
        }
        LLUIColorTable::Params params;
        LLXUIParser parser;
        parser.readXUI(root, params, utf8(file), true);

        LLUIColorTable& table = LLUIColorTable::instance();
        std::vector<std::pair<std::string, LLColor4>> changes;
        for (const LLUIColorTable::ColorEntryParams& entry : params.color_entries)
        {
            const std::string& name = entry.name();
            if (!entry.color.value.isChosen() || !table.colorExists(name))
            {
                continue;
            }
            const LLColor4 color = entry.color.value();
            if (table.getColor(name).get() != color)
            {
                changes.emplace_back(name, color);
            }
        }
        if (changes.empty())
        {
            return;
        }

        // fix-round68 2: our colors.xml backed up first (the confirmation says what was there is
        // kept), as the key bindings are; no backup, no change. None of ours: the defaults, nothing to keep.
        const fs::path ours = fsyspath(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "colors.xml"));
        if (fs::is_regular_file(ours, ec))
        {
            const fs::path backup = backupPath(ours.parent_path(), BACKUP_COLORS);
            if (!fs::copy_file(ours, backup, fs::copy_options::none, ec))
            {
                ++c.result.errors;
                c.note("cannot back up " + utf8(ours) + " to " + utf8(backup) + ": " + ec.message()
                       + " (left as they were)");
                return;
            }
            pruneBackups(ours.parent_path(), BACKUP_COLORS, backup);
        }
        for (const auto& [name, color] : changes)
        {
            table.setColor(name, color);
        }
        table.saveUserSettings();
        c.result.settings += (U32)changes.size();
    }

    // Reads the saved logins of a copy of the source's bin_conf.dat.
    class SourceLogins : public LLSecAPIBasicHandler
    {
    public:
        explicit SourceLogins(const std::string& file) : LLSecAPIBasicHandler(file, std::string()) {}
        // fix-round52 3: the store writes its data back to its file when it goes; an undefined
        // map makes it remove the file instead. So the copy is never written again (encrypted
        // for this computer), read or not.
        ~SourceLogins() { mProtectedDataMap = LLSD(); }
        const LLSD& data() const { return mProtectedDataMap; }
    };

    // Removes a file when it goes out of scope, on every way out (fix-round52 3).
    struct RemoveWhenDone
    {
        const fs::path path;
        ~RemoveWhenDone()
        {
            std::error_code ec;
            fs::remove(path, ec);
        }
    };

    void importLogins(Context& c)
    {
        const fs::path file = c.from / "user_settings" / "bin_conf.dat";
        std::error_code ec;
        if (!fs::is_regular_file(file, ec) || !gSecAPIHandler)
        {
            return;
        }
        // The store writes its file back when it goes: it reads a copy, never the source's file.
        const fs::path copy = fsyspath(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, LOGINS_COPY));
        // Never left behind (the source's saved logins): not after a copy cut short, nor after
        // a store that could not read it, nor when anything below throws.
        const RemoveWhenDone remove_copy{ copy };
        if (!fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec))
        {
            ++c.result.errors;
            c.note("cannot copy " + utf8(file) + ": " + ec.message());
            return;
        }

        LLSD theirs;
        {
            LLPointer<SourceLogins> store = new SourceLogins(utf8(copy));
            try
            {
                store->init();      // decrypted with this computer's id, as ours is
                theirs = store->data();
            }
            catch (...)
            {
                ++c.result.errors;
                c.note("cannot read the saved logins of " + utf8(file) + " (made on another computer?)");
            }
        }
        fs::remove(copy, ec);
        if (!theirs.isMap())
        {
            return;
        }

        U32 added = 0;
        for (LLSD::map_const_iterator type_it = theirs.beginMap(); type_it != theirs.endMap(); ++type_it)
        {
            const std::string& type = type_it->first;
            if (!type_it->second.isMap())
            {
                continue;
            }
            const bool per_user = isOneOf(type, PER_USER_LOGIN_DATA);
            for (LLSD::map_const_iterator id_it = type_it->second.beginMap(); id_it != type_it->second.endMap(); ++id_it)
            {
                const std::string& id = id_it->first;     // the grid
                const LLSD ours = gSecAPIHandler->getProtectedData(type, id);
                if (per_user && id_it->second.isMap())
                {
                    for (LLSD::map_const_iterator user_it = id_it->second.beginMap(); user_it != id_it->second.endMap(); ++user_it)
                    {
                        if (!ours.isMap() || !ours.has(user_it->first))
                        {
                            gSecAPIHandler->addToProtectedMap(type, id, user_it->first, user_it->second);
                            added += type == "login_list" ? 1 : 0;
                        }
                    }
                }
                else if (ours.isUndefined())
                {
                    gSecAPIHandler->setProtectedData(type, id, id_it->second);
                }
            }
        }
        gSecAPIHandler->syncProtectedMap();
        c.result.logins += added;

        // the login screen's list of names
        if (LLStartUp::getStartupState() <= STATE_LOGIN_WAIT)
        {
            LLPanelLogin::updateServer();
        }
    }

    void importKeys(Context& c)
    {
        const fs::path theirs = c.from / "user_settings" / "key_bindings.xml";
        std::error_code ec;
        if (!fs::is_regular_file(theirs, ec))
        {
            return;     // they use the default keys: ours stay
        }
        const std::string user_file = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings.xml");
        const fs::path ours = fsyspath(user_file);
        const fs::path temp = fsyspath(plus::tempPathFor(user_file));   // per process: another viewer may write it too

        // The new file arrives whole: copied beside ours, then moved over it.
        const bool had_ours = fs::is_regular_file(ours, ec);
        const fs::path backup = had_ours ? backupPath(ours.parent_path(), BACKUP_KEYS) : fs::path();
        if (!fs::copy_file(theirs, temp, fs::copy_options::overwrite_existing, ec)
            // fix-round53 1: never over a backup there (all names of this second in use)
            || (had_ours && !fs::copy_file(ours, backup, fs::copy_options::none, ec)))
        {
            ++c.result.errors;
            c.note("cannot copy the key bindings: " + ec.message());
            fs::remove(temp, ec);
            return;
        }
        // copy_file keeps their file's time: dated now, so that until the move it is never taken for
        // a leftover of a crash (another viewer's removeStaleTemps() takes ones an hour old)
        fs::last_write_time(temp, fs::file_time_type::clock::now(), ec);
        if (had_ours)
        {
            pruneBackups(ours.parent_path(), BACKUP_KEYS, backup);  // once the new backup is there
        }
        fs::permissions(temp, fs::perms::owner_write, fs::perm_options::add, ec);
        fs::rename(temp, ours, ec);
        if (ec)
        {
            ++c.result.errors;
            c.note("cannot replace " + user_file + ": " + ec.message());
            fs::remove(temp, ec);
            return;
        }
        plus::removeStaleTemps(user_file);   // what a crash while bringing them over left

        if (gViewerInput.loadBindingsXML(user_file) <= 0)
        {
            // as LLAppViewer::loadKeyBindings(): never a session without keys
            ++c.result.errors;
            c.note("cannot load the key bindings of " + utf8(theirs) + "; ours are back");
            if (had_ours)
            {
                fs::copy_file(backup, ours, fs::copy_options::overwrite_existing, ec);
            }
            else
            {
                fs::remove(ours, ec);
            }
            if (!had_ours || gViewerInput.loadBindingsXML(user_file) <= 0)
            {
                gViewerInput.loadBindingsXML(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "key_bindings.xml"));
            }
            return;
        }
        c.result.keys = true;

        // The photo tools' keys, where their file has none (it was saved by a viewer without them).
        gSavedSettings.setString("PlusPhotoKeysCommands", std::string());
        plus::photomode::addDefaultKeys();
    }

    // Our folder of the account that is logged in now; empty before the login.
    fs::path loggedInAccount()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || gDirUtilp->getLindenUserDir().empty())
        {
            return fs::path();
        }
        return fsyspath(gDirUtilp->getLindenUserDir());
    }

    // Our account folder of the account that is logged in now (its settings are in this session).
    bool isLoggedIn(const fs::path& our_account)
    {
        const fs::path logged_in = loggedInAccount();
        return !logged_in.empty() && folderKey(utf8(our_account)) == folderKey(utf8(logged_in));
    }

    void importAccounts(Context& c)
    {
        for (const fs::path& account : accountFolders(c.from))
        {
            const fs::path ours = c.to / account.filename();
            const fs::path settings = account / ACCOUNT_SETTINGS_FILE;
            std::error_code ec;
            if (isLoggedIn(ours))
            {
                // fix-round52 1: not changed without a backup (its files below are still
                // copied: only where it has none, nothing of its own is replaced).
                // fix-round53 1: and no backup when they have no settings to bring.
                if (fs::is_regular_file(settings, ec)
                    && backUpGroup(gSavedPerAccountSettings, ours, BACKUP_ACCOUNT_SETTINGS, c))
                {
                    c.result.settings += applyToGroup(gSavedPerAccountSettings, settings, true, false, c);
                }
            }
            else
            {
                c.result.settings += applyToAccountFile(settings, ours / ACCOUNT_SETTINGS_FILE, c);
            }

            for (fs::directory_iterator it(account, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code fec;
                const fs::file_status status = it->symlink_status(fec);
                const std::string name = lowered(utf8(it->path().filename()));
                if (fs::is_directory(status) && name == PRESETS)
                {
                    copyPresetsIfMissing(it->path(), ours / PRESETS, c);
                }
                else if (fs::is_regular_file(status) && name != ACCOUNT_SETTINGS_FILE && isAccountFile(name))
                {
                    copyIfMissing(it->path(), ours / it->path().filename(), c);
                }
            }
        }
    }

    // An account's chat log folder: InstantMessageLogPath of its settings_per_account.xml (the
    // viewer writes the folder there at the first login), or else the viewer's folder; the
    // account's logs are in a folder of the account's name there.
    std::string chatLogPath(const fs::path& account_settings)
    {
        LLSD settings;
        if (readSettings(account_settings, settings) && settings.has(CHAT_LOG_SETTING))
        {
            return settings[CHAT_LOG_SETTING]["Value"].asString();
        }
        return std::string();
    }

    fs::path chatFolder(const fs::path& root, const std::string& log_path, const fs::path& account_name)
    {
        return (log_path.empty() ? root : fs::path(fsyspath(log_path))) / account_name;
    }

    // Each account's logs, from its chat log folder in the source to its chat log folder here
    // (both may be set apart from the viewer's folder, account by account). After the accounts'
    // settings: ours may have just been brought over.
    void importChat(Context& c)
    {
        for (const fs::path& account : accountFolders(c.from))
        {
            const fs::path name = account.filename();
            const fs::path theirs = chatFolder(c.from, chatLogPath(account / ACCOUNT_SETTINGS_FILE), name);
            const fs::path our_account = c.to / name;
            const std::string our_path = isLoggedIn(our_account)
                                         ? gSavedPerAccountSettings.getString(CHAT_LOG_SETTING)
                                         : chatLogPath(our_account / ACCOUNT_SETTINGS_FILE);
            const fs::path ours = chatFolder(c.to, our_path, name);
            if (folderKey(utf8(theirs)) == folderKey(utf8(ours)))
            {
                c.note("the chat logs are already shared: " + utf8(ours));
                continue;
            }

            std::error_code ec;
            for (fs::directory_iterator it(theirs, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code fec;
                const std::string file = lowered(utf8(it->path().filename()));
                if (fs::is_regular_file(it->symlink_status(fec)) && isTranscript(file))
                {
                    queueIfMissing(*it, ours / it->path().filename(), c);
                }
            }
        }
    }

    // The accounts the presets of the source's user_settings go to (Firestorm keeps them there,
    // FIRE-19810; this viewer reads an account's own, as the official viewer does): our folder of
    // each account of the source (a folder with its settings_per_account.xml: a folder of
    // something else is not made an account here), and the account logged in.
    std::vector<fs::path> presetAccounts(const Context& c)
    {
        std::vector<fs::path> accounts;
        auto add = [&accounts](const fs::path& account)
        {
            const std::string key = folderKey(utf8(account));
            for (const fs::path& known : accounts)
            {
                if (folderKey(utf8(known)) == key)
                {
                    return;
                }
            }
            accounts.push_back(account);
        };
        for (const fs::path& account : accountFolders(c.from))
        {
            std::error_code ec;
            if (fs::is_regular_file(account / ACCOUNT_SETTINGS_FILE, ec))
            {
                add(c.to / account.filename());
            }
        }
        const fs::path logged_in = loggedInAccount();
        if (!logged_in.empty())
        {
            add(logged_in);
        }
        return accounts;
    }

    // user_settings's folders (poses, environments, dictionaries, ...), its presets (into the
    // accounts' folders) and the login screen's favorites.
    void importFiles(Context& c)
    {
        const fs::path from = c.from / "user_settings";
        const fs::path to = c.to / "user_settings";
        std::error_code ec;
        for (fs::directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            const fs::file_status status = it->symlink_status(fec);
            const std::string name = lowered(utf8(it->path().filename()));
            if (fs::is_directory(status) && name == PRESETS)
            {
                // Not into our user_settings: this viewer would not list them there.
                const std::vector<fs::path> accounts = presetAccounts(c);
                if (accounts.empty())
                {
                    c.note("the presets of " + utf8(it->path())
                           + " are not copied: no account to put them in (log in once, then bring them over again)");
                }
                for (const fs::path& account : accounts)
                {
                    copyPresetsIfMissing(it->path(), account / PRESETS, c);
                }
            }
            else if (fs::is_directory(status) && !isOneOf(name, SKIPPED_DIRS))
            {
                copyTreeIfMissing(it->path(), to / it->path().filename(), c);
            }
            else if (fs::is_regular_file(status) && LLStringUtil::startsWith(name, "stored_favorites") && LLStringUtil::endsWith(name, ".xml"))
            {
                copyIfMissing(it->path(), to / it->path().filename(), c);
            }
        }
    }

    // The name of the graphics preset in a file: as the list names it (LLPresetsManager::
    // loadPresetNamesFromDir: the file's name unescaped, without .xml), so that a file named by
    // hand ("My Preset.xml", not escaped) is found too. Empty: not a preset file.
    std::string graphicPresetName(const fs::path& file)
    {
        const std::string name = utf8(file.filename());
        if (!LLStringUtil::endsWith(lowered(name), ".xml"))
        {
            return std::string();
        }
        return LLURI::unescape(name.substr(0, name.size() - 4));
    }

    // The account has the graphics preset of that name.
    bool accountHasGraphicPreset(const fs::path& account, const std::string& name)
    {
        std::error_code ec;
        for (fs::directory_iterator it(account / PRESETS / GRAPHIC_PRESETS, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (it->is_regular_file(fec) && graphicPresetName(it->path()) == name)
            {
                return true;
            }
        }
        return false;
    }

    // The graphics preset of that name is there, or is being copied there: in the account logged
    // in; before the login, in any account of ours (which one logs in next is not known).
    bool graphicPresetThere(const std::string& name, const Context& c)
    {
        if (name == DEFAULT_PRESET)
        {
            return true;
        }
        const fs::path logged_in = loggedInAccount();
        const std::vector<fs::path> accounts = logged_in.empty() ? accountFolders(c.to) : std::vector<fs::path>{ logged_in };
        for (const fs::path& account : accounts)
        {
            if (accountHasGraphicPreset(account, name))
            {
                return true;
            }
        }
        const std::string to_key = folderKey(utf8(c.to));
        const std::string logged_in_key = logged_in.empty() ? std::string() : folderKey(utf8(logged_in));
        for (const Copy& copy : c.copies)
        {
            // <ours>\<account>\presets\graphic\<file>
            const fs::path folder = copy.to.parent_path();
            const fs::path account = folder.parent_path().parent_path();
            if (lowered(utf8(folder.filename())) != GRAPHIC_PRESETS || lowered(utf8(folder.parent_path().filename())) != PRESETS
                || folderKey(utf8(account.parent_path())) != to_key
                || (!logged_in.empty() && folderKey(utf8(account)) != logged_in_key))
            {
                continue;
            }
            if (graphicPresetName(copy.to) == name)
            {
                return true;
            }
        }
        return false;
    }

    // After the settings and the files: the graphics preset in use brought over (PresetGraphicActive,
    // global) is kept only when that preset is there or being copied there. It may not be: the
    // account's files were not brought, Firestorm's presets had no account to go to, or it names
    // another language's copy of a Charis preset (never copied). Such a copy's name is
    // plus/presets' to settle: this viewer's copy that matches the settings, or none (now when
    // logged in; else at the login, when the copies are made). Any other is cleared, as the
    // official viewer clears the name of a preset deleted. A source with no preset in use (its
    // settings.xml leaves the setting out) whose graphics settings came in leaves none in use
    // here either: ours named the settings that were replaced.
    void activePresetChecked(Context& c)
    {
        if (!c.active_preset_before)
        {
            return;     // the graphics were not brought
        }
        const std::string active = gSavedSettings.getString(ACTIVE_PRESET_SETTING);
        if (active.empty())
        {
            return;
        }
        if (active == *c.active_preset_before)
        {
            // Ours, unchanged. A Charis copy's name is cleared by plus/presets once the
            // settings no longer match it (checkActivePreset()); any other is cleared here.
            if (!c.source_names_active_preset && c.graphics_changed && !LLStringUtil::startsWith(active, CHARIS_PRESET_PREFIX))
            {
                gSavedSettings.setString(ACTIVE_PRESET_SETTING, std::string());
                c.note("no graphics preset was in use in the source: " + active + " is not in use now");
            }
            return;
        }
        if (graphicPresetThere(active, c))
        {
            if (active != DEFAULT_PRESET)
            {
                c.active_preset_kept = active;  // looked at again when the copies are done
            }
            return;
        }
        if (LLStringUtil::startsWith(active, CHARIS_PRESET_PREFIX))
        {
            plus::presets::recheckActivePresetName();
            c.note("the graphics preset in use, " + active
                   + ", is not here (the Charis presets are retired, research/183): none is in use now"
                   + (loggedInAccount().empty() ? " (at the login)" : ""));
            return;
        }
        gSavedSettings.setString(ACTIVE_PRESET_SETTING, std::string());
        c.note("the graphics preset in use, " + active + ", is not here: none is in use now");
    }
}

namespace
{
    using namespace plus::importsettings;

    // The files being copied, a few each frame.
    struct Job
    {
        Result result;
        std::vector<Copy> copies;
        size_t next = 0;
        std::string active_preset_kept;     // Context::active_preset_kept
        std::function<void(U32, U32)> progress;
        std::function<void(const Result&)> done;
    };
    std::unique_ptr<Job> sJob;

    void logResult(const Result& result)
    {
        LL_INFOS("ImportSettings") << result.settings << " settings, " << result.logins << " saved logins, "
                                   << (result.keys ? "key bindings, " : "") << result.files << " files ("
                                   << result.kept << " we had already were left, " << result.over_limit
                                   << " over the limit), " << result.errors << " errors"
                                   << (result.stopped ? "; stopped by the viewer quitting" : "") << LL_ENDL;
        for (const std::string& line : result.notes)
        {
            LL_INFOS("ImportSettings") << "  " << line << LL_ENDL;
        }
    }

    void finishJob()
    {
        const std::unique_ptr<Job> job = std::move(sJob);
        logResult(job->result);
        // Presets copied into the folder of the account logged in: the lists that show them
        // (the presets pulldown, Preferences) read the folder again. Not before the login: the
        // listeners read the account's folder, which is not named yet.
        if (job->result.files && !loggedInAccount().empty())
        {
            LLPresetsManager::getInstance()->triggerChangeSignal();
        }
        // The graphics preset in use, kept for a file being copied (or there in some account
        // before the login): in the account logged in now? A copy may have failed, and a login
        // meanwhile may be of an account without it. Before the login it stays as it is.
        const fs::path logged_in = loggedInAccount();
        const std::string& kept = job->active_preset_kept;
        if (!kept.empty() && !logged_in.empty() && gSavedSettings.getString(ACTIVE_PRESET_SETTING) == kept
            && !accountHasGraphicPreset(logged_in, kept))
        {
            if (LLStringUtil::startsWith(kept, CHARIS_PRESET_PREFIX))
            {
                plus::presets::recheckActivePresetName();
            }
            else
            {
                gSavedSettings.setString(ACTIVE_PRESET_SETTING, std::string());
            }
            LL_INFOS("ImportSettings") << "The graphics preset in use, " << kept << ", is not in "
                                       << utf8(logged_in) << ": " << gSavedSettings.getString(ACTIVE_PRESET_SETTING)
                                       << " is named instead (empty: none)" << LL_ENDL;
        }
        if (job->done)
        {
            job->done(job->result);
        }
    }

    // On idle: copies files until COPY_SECONDS_PER_FRAME has gone. True when there are no more
    // (the callback goes).
    bool copySome()
    {
        if (!sJob)
        {
            return true;
        }
        Job& job = *sJob;
        if (LLApp::isExiting())
        {
            // What is copied stays; the rest is not (a file is copied whole or not at all).
            job.result.stopped = true;
            logResult(job.result);
            sJob.reset();
            return true;
        }
        const F64 start = LLTimer::getTotalSeconds();
        try
        {
            while (job.next < job.copies.size())
            {
                const Copy& copy = job.copies[job.next++];
                copyFile(copy.from, copy.to, job.result);
                if (LLTimer::getTotalSeconds() - start >= COPY_SECONDS_PER_FRAME)
                {
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            ++job.result.errors;
            addNote(job.result, std::string("stopped copying: ") + e.what());
            job.next = job.copies.size();
        }
        catch (...)
        {
            // Whatever it was, the job ends here (sJob does not stay, and running() is false again).
            ++job.result.errors;
            addNote(job.result, "stopped copying: an unknown error");
            job.next = job.copies.size();
        }
        if (job.next >= job.copies.size())
        {
            finishJob();
            return true;
        }
        if (job.progress)
        {
            job.progress((U32)job.next, (U32)job.copies.size());
        }
        return false;
    }
}

bool plus::importsettings::running()
{
    return sJob != nullptr;
}

bool plus::importsettings::start(const Source& source, const Choice& choice,
                                 std::function<void(U32 copied, U32 total)> progress,
                                 std::function<void(const Result& result)> done)
{
    if (sJob)
    {
        return false;
    }
    sJob = std::make_unique<Job>();
    sJob->progress = std::move(progress);
    sJob->done = std::move(done);
    Result& result = sJob->result;
    Context c{ source, choice, result, fsyspath(source.root), fsyspath(gDirUtilp->getOSUserAppDir()) };
    addRoot(c.roots, c.from, utf8(c.to));
    // A bare root ("D:\") has no name to find its cache folder by; without this check
    // the whole of %LOCALAPPDATA% would count as its cache.
    const fs::path from_name = folderName(c.from);
    if (!gDirUtilp->getOSCacheDir().empty() && !from_name.empty())
    {
        const fs::path cache_root = fsyspath(gDirUtilp->getOSCacheDir());
        addRoot(c.roots, cache_root / from_name, gDirUtilp->getCacheDir());
    }
    // Log privacy: the accounts' folders on both sides (and their chat log folders) are written
    // as [accountN] in the log, before a note names one (most of this runs before any log in).
    // Only folders with an account's settings file: not beams, skins, subtitle and the like.
    // Each account on its own: a path that does not convert (or no memory) skips that account only, and
    // the import goes on (sJob does not stay behind). The folders not registered are written as they are.
    auto registerAccount = [](const fs::path& root, const fs::path& account, const fs::path& other_root)
    {
        try
        {
            std::error_code ec;
            const fs::path settings = account / ACCOUNT_SETTINGS_FILE;
            if (!fs::is_regular_file(settings, ec))
            {
                return;
            }
            const fs::path name = account.filename();
            LLError::addPrivateAccountDir(utf8(account));
            if (!other_root.empty())
            {
                LLError::addPrivateAccountDir(utf8(other_root / name));   // where it is brought over to
            }
            LLError::addPrivateAccountDir(utf8(chatFolder(root, chatLogPath(settings), name)));
        }
        catch (...)
        {
        }
    };
    auto registerAccounts = [&registerAccount](const fs::path& root, const fs::path& other_root)
    {
        std::vector<fs::path> accounts;
        try
        {
            accounts = accountFolders(root);
        }
        catch (...)
        {
        }
        for (const fs::path& account : accounts)
        {
            registerAccount(root, account, other_root);
        }
    };
    registerAccounts(c.from, c.to);
    registerAccounts(c.to, fs::path());

    LL_INFOS("ImportSettings") << "Bringing settings over from " << source.name << " (" << source.root << ")"
                               << (choice.general ? " general" : "") << (choice.graphics ? " graphics" : "")
                               << (choice.logins ? " logins" : "") << (choice.keys ? " keys" : "")
                               << (choice.accounts ? " accounts" : "") << (choice.chat ? " chat" : "")
                               << (choice.files ? " files" : "") << LL_ENDL;
    try
    {
        if (choice.general || choice.graphics)
        {
            importGeneral(c);
        }
        if (choice.general)
        {
            importColors(c);
        }
        if (choice.logins)
        {
            importLogins(c);
        }
        if (choice.keys)
        {
            importKeys(c);
        }
        if (choice.accounts)
        {
            importAccounts(c);
        }
        if (choice.chat)
        {
            importChat(c);      // after the settings: our chat log folder may have just changed
        }
        if (choice.files)
        {
            importFiles(c);
        }
    }
    catch (const std::exception& e)
    {
        ++result.errors;
        c.note(std::string("stopped: ") + e.what());
    }
    catch (...)
    {
        // importGeneral() passes on whatever stopped it: the job still ends below (sJob does
        // not stay, which would refuse every later start()).
        ++result.errors;
        c.note("stopped: an unknown error");
    }
    // Also when it stopped midway: the settings set before then stay.
    try
    {
        activePresetChecked(c);
    }
    catch (const std::exception& e)
    {
        ++result.errors;
        c.note(std::string("cannot check the graphics preset in use: ") + e.what());
    }
    catch (...)
    {
        ++result.errors;
        c.note("cannot check the graphics preset in use: an unknown error");
    }

    sJob->active_preset_kept = c.active_preset_kept;
    if (c.copies.empty())
    {
        finishJob();
        return true;
    }
    LL_INFOS("ImportSettings") << c.copies.size() << " files (" << (c.copies_bytes >> 20) << " MB) to copy, a few each frame" << LL_ENDL;
    sJob->copies = std::move(c.copies);
    if (sJob->progress)
    {
        sJob->progress(0, (U32)sJob->copies.size());
    }
    doOnIdleRepeating(&copySome);
    return true;
}
