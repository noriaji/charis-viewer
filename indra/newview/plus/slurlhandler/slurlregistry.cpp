/**
 * @file slurlregistry.cpp
 * @brief Which program opens secondlife:// links from the browser, and making
 *        this viewer that program (or undoing it) for the current Windows user.
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

// research/237-slurl-handler.md. How Windows finds the program for secondlife://:
//   1. HKCU\...\Shell\Associations\UrlAssociations\<scheme>\UserChoice\ProgId (Windows' default
//      apps; hashed, programs cannot write it), then HKCR\<ProgId>\shell\open\command
//   2. else HKCU\Software\Classes\<scheme>\shell\open\command (per user: Velopack installs, us)
//   3. else HKLM\Software\Classes\<scheme>\shell\open\command (installers run as administrator:
//      the official NSIS installer, Firestorm's)
// We write 2 only, for this user, when the user presses the button: four of the values the
// installers write (the key's default, "URL Protocol", DefaultIcon, shell\open\command), and
// shell\open FriendlyAppName removed (an old one would name the wrong program; Windows then
// shows our exe's FileDescription), nothing else. The command is "<this exe>" -url "%1": the
// viewer's "url" option is its last one (cmd_line.xml last_option), so nothing after the
// link can be passed as another option (SL-55321). What those five values were before is kept
// in HKCU\Software\CharisViewer\SLURLBackup\<scheme> (each value with its own type, and
// "<name>Type", a DWORD, for the installer's uninstaller: tools/make-release-installer.sh;
// "LongValue" 1 when one is too long for it), with "OwnerCommand", the command we wrote: while
// HKCU still has that command, a Charis Viewer holds the links over this backup. undo() puts
// the values back, only while the command is still ours.

#include "llviewerprecompiledheaders.h"

#include "slurlregistry.h"

#include "plus/plus.h"

#include "llstring.h"

#if LL_WINDOWS
#include "llwin32headers.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <vector>

#pragma comment(lib, "version.lib")   // GetFileVersionInfoW, VerQueryValueW
#endif

namespace plus
{
    namespace slurlhandler
    {
#if LL_WINDOWS
        namespace
        {
            constexpr const wchar_t* SCHEMES[] = { L"secondlife", L"x-grid-location-info" };
            constexpr const wchar_t* MAIN_SCHEME = L"secondlife";

            const std::wstring CLASSES     = L"Software\\Classes\\";
            const std::wstring BACKUP_ROOT = L"Software\\CharisViewer\\SLURLBackup";
            const std::wstring USER_CHOICE = L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\";
            constexpr const wchar_t* DESCRIPTION   = L"URL:Second Life";  // as the official installer writes it
            constexpr const wchar_t* FRIENDLY_NAME = L"Charis Viewer";    // recognised only (not written)
            constexpr const char*    OUR_NAME      = "Charis Viewer";     // our exe's FileDescription

            constexpr DWORD MAX_VALUE_BYTES = 64 * 1024;   // a command line is far shorter
            constexpr DWORD MAX_STRING_BYTES      = 32767 * sizeof(wchar_t);  // the longest value put back
            constexpr DWORD UNINSTALLER_MAX_BYTES = 1000 * sizeof(wchar_t);   // NSIS_MAX_STRLEN is 1024
            constexpr const wchar_t* OWNER_COMMAND = L"OwnerCommand";  // in the backup: the command the Charis wrote
            constexpr const wchar_t* LONG_VALUE    = L"LongValue";     // in the backup: DWORD 1, too long for NSIS
            constexpr size_t MAX_NAME_BYTES = 192;         // a shown name, UTF-8

            // The values we write, and the name each is kept under in the backup.
            struct Slot
            {
                const wchar_t* subkey;   // under HKCU\Software\Classes\<scheme>
                const wchar_t* value;    // nullptr: the key's default value
                const wchar_t* saved;    // its name in the backup
            };
            constexpr Slot SLOTS[] = {
                { L"",                      nullptr,             L"Default" },
                { L"",                      L"URL Protocol",     L"URLProtocol" },
                { L"\\DefaultIcon",         nullptr,             L"Icon" },
                { L"\\shell\\open\\command", nullptr,            L"Command" },
                { L"\\shell\\open",         L"FriendlyAppName",  L"FriendlyAppName" },
            };
            // Removed after an undo when nothing is left in them, deepest first.
            constexpr const wchar_t* SUBKEYS[] = {
                L"\\shell\\open\\command", L"\\shell\\open", L"\\shell", L"\\DefaultIcon", L""
            };

            enum class Read { Found, Absent, Error };

            struct Raw
            {
                DWORD             type = REG_NONE;
                std::vector<BYTE> data;
            };

            Read readRaw(HKEY root, const std::wstring& path, const wchar_t* name, Raw& out)
            {
                HKEY key = nullptr;
                LSTATUS rc = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key);
                if (rc == ERROR_FILE_NOT_FOUND)
                {
                    return Read::Absent;
                }
                if (rc != ERROR_SUCCESS)
                {
                    return Read::Error;
                }
                DWORD type = REG_NONE;
                DWORD size = 0;
                rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
                if (rc == ERROR_SUCCESS && size <= MAX_VALUE_BYTES)
                {
                    out.data.resize(size);
                    rc = RegQueryValueExW(key, name, nullptr, &type, size ? out.data.data() : nullptr, &size);
                    out.data.resize(size);
                    out.type = type;
                }
                else if (rc == ERROR_SUCCESS)
                {
                    rc = ERROR_MORE_DATA;   // too big to be one of these values
                }
                RegCloseKey(key);
                if (rc == ERROR_FILE_NOT_FOUND)
                {
                    return Read::Absent;
                }
                return rc == ERROR_SUCCESS ? Read::Found : Read::Error;
            }

            bool isString(DWORD type)
            {
                return type == REG_SZ || type == REG_EXPAND_SZ;
            }

            // A string value; expand tells whether it is REG_EXPAND_SZ. False when absent or not a string.
            bool readString(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring& out, bool& expand)
            {
                Raw raw;
                if (readRaw(root, path, name, raw) != Read::Found || !isString(raw.type))
                {
                    return false;
                }
                out.assign(reinterpret_cast<const wchar_t*>(raw.data.data()), raw.data.size() / sizeof(wchar_t));
                while (!out.empty() && out.back() == L'\0')
                {
                    out.pop_back();
                }
                expand = raw.type == REG_EXPAND_SZ;
                return true;
            }

            bool writeRaw(const std::wstring& path, const wchar_t* name, DWORD type, const BYTE* data, DWORD size)
            {
                HKEY key = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                {
                    return false;
                }
                const LSTATUS rc = RegSetValueExW(key, name, 0, type, data, size);
                RegCloseKey(key);
                return rc == ERROR_SUCCESS;
            }

            bool writeString(const std::wstring& path, const wchar_t* name, const std::wstring& value)
            {
                return writeRaw(path, name, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            }

            bool deleteValue(const std::wstring& path, const wchar_t* name)
            {
                HKEY key = nullptr;
                LSTATUS rc = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &key);
                if (rc == ERROR_FILE_NOT_FOUND)
                {
                    return true;
                }
                if (rc != ERROR_SUCCESS)
                {
                    return false;
                }
                rc = RegDeleteValueW(key, name);
                RegCloseKey(key);
                return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
            }

            // Removes the key only when it holds no value and no subkey.
            void deleteKeyIfEmpty(const std::wstring& path)
            {
                HKEY key = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
                {
                    return;
                }
                DWORD subkeys = 1;
                DWORD values  = 1;
                const LSTATUS rc = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                                    &values, nullptr, nullptr, nullptr, nullptr);
                RegCloseKey(key);
                if (rc == ERROR_SUCCESS && subkeys == 0 && values == 0)
                {
                    RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
                }
            }

            bool iequal(const std::wstring& a, const std::wstring& b)
            {
                return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                            b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
            }

            bool exists(const std::wstring& file)
            {
                const DWORD attributes = GetFileAttributesW(file.c_str());
                return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            }

            // Full form of a path, from the string alone (no file access). Empty on failure.
            std::wstring fullPath(const std::wstring& path)
            {
                if (path.empty() || path.size() >= 32767)
                {
                    return {};
                }
                std::wstring full(32768, L'\0');
                const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
                if (n == 0 || n >= full.size())
                {
                    return {};
                }
                full.resize(n);
                return full;
            }

            // A path on another computer (\\server\share or a network drive): reading it from the main
            // thread can wait for a sleeping or gone NAS, so such a registration is shown without
            // touching the file (review low 6).
            bool remote(const std::wstring& full)
            {
                if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\')
                {
                    return true;
                }
                if (full.size() >= 3 && full[1] == L':')
                {
                    const wchar_t root[] = { full[0], L':', L'\\', L'\0' };
                    return GetDriveTypeW(root) == DRIVE_REMOTE;
                }
                return false;
            }

            // Full, long form of a path (so two spellings of one file compare equal). Empty on failure.
            // Touches the file system: not for remote() paths.
            std::wstring normalize(const std::wstring& path)
            {
                const std::wstring full = fullPath(path);
                if (full.empty())
                {
                    return {};
                }
                std::wstring longer(32768, L'\0');
                DWORD n = 0;
                n = GetLongPathNameW(full.c_str(), longer.data(), static_cast<DWORD>(longer.size()));
                if (n == 0 || n >= longer.size())
                {
                    return full;   // not there (a missing exe): the full form is enough to compare
                }
                longer.resize(n);
                return longer;
            }

            std::wstring ourExeW()
            {
                const std::wstring exe = plus::ownExePath();
                return exe.empty() ? std::wstring() : normalize(exe);
            }

            // Our exe may go into "<exe>" -url "%1" only when it cannot end the quotes or be read
            // by the shell as a parameter: an absolute path to an existing .exe, without '"'
            // (cannot be in a Windows file name anyway), '%' (the shell puts the link in place
            // of %1, %2, ... even in a REG_SZ) or control characters.
            bool safeForCommand(const std::wstring& exe)
            {
                if (exe.size() < 8 || exe.size() >= 32000)
                {
                    return false;
                }
                for (wchar_t c : exe)
                {
                    if (c < 0x20 || c == 0x7f || c == L'"' || c == L'%')
                    {
                        return false;
                    }
                }
                const bool drive = std::iswalpha(exe[0]) && exe[1] == L':' && exe[2] == L'\\';
                const bool unc   = exe[0] == L'\\' && exe[1] == L'\\';
                if (!drive && !unc)
                {
                    return false;
                }
                if (!iequal(exe.substr(exe.size() - 4), L".exe"))
                {
                    return false;
                }
                return exists(exe);
            }

            std::wstring commandFor(const std::wstring& exe)
            {
                return L"\"" + exe + L"\" -url \"%1\"";
            }

            // The program a shell\open\command names: in quotes, else up to ".exe", else the first word.
            std::wstring exeOfCommand(std::wstring command, bool expand)
            {
                if (expand)
                {
                    const DWORD needed = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
                    if (needed > 0 && needed < 32768)
                    {
                        std::wstring expanded(needed, L'\0');
                        const DWORD n = ExpandEnvironmentStringsW(command.c_str(), expanded.data(), needed);
                        if (n > 0 && n <= needed)
                        {
                            expanded.resize(n - 1);
                            command = expanded;
                        }
                    }
                }
                const size_t start = command.find_first_not_of(L" \t");
                if (start == std::wstring::npos)
                {
                    return {};
                }
                command.erase(0, start);
                if (command[0] == L'"')
                {
                    const size_t end = command.find(L'"', 1);
                    return command.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
                }
                std::wstring lower(command);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
                const size_t dot_exe = lower.find(L".exe");
                if (dot_exe != std::wstring::npos)
                {
                    return command.substr(0, dot_exe + 4);
                }
                return command.substr(0, command.find(L' '));
            }

            // Text from another program, made safe to show: no control characters, trimmed, short.
            std::string shown(const std::wstring& text)
            {
                std::wstring clean;
                for (wchar_t c : text)
                {
                    if (c == L'\0')
                    {
                        break;
                    }
                    if (c >= 0x20 && c != 0x7f)
                    {
                        clean.push_back(c);
                    }
                }
                std::string out = ll_convert<std::string>(clean);
                LLStringUtil::trim(out);
                if (out.size() > MAX_NAME_BYTES)
                {
                    out = utf8str_truncate(out, (S32)MAX_NAME_BYTES);   // at a character boundary
                }
                return out;
            }

            // FileDescription (else ProductName) from the exe's version resource.
            std::string versionName(const std::wstring& exe)
            {
                DWORD handle = 0;
                const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
                if (size == 0 || size > 4 * 1024 * 1024)
                {
                    return {};
                }
                std::vector<BYTE> block(size);
                if (!GetFileVersionInfoW(exe.c_str(), 0, size, block.data()))
                {
                    return {};
                }
                std::vector<std::wstring> tables;
                struct LangCodePage { WORD language; WORD codepage; };
                LangCodePage* translation = nullptr;
                UINT length = 0;
                if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translation), &length)
                    && translation && length >= sizeof(LangCodePage))
                {
                    wchar_t table[16];
                    swprintf(table, std::size(table), L"%04x%04x", translation[0].language, translation[0].codepage);
                    tables.emplace_back(table);
                }
                tables.emplace_back(L"040904b0");
                tables.emplace_back(L"040904e4");
                for (const wchar_t* field : { L"FileDescription", L"ProductName" })
                {
                    for (const std::wstring& table : tables)
                    {
                        const std::wstring query = L"\\StringFileInfo\\" + table + L"\\" + field;
                        wchar_t* text = nullptr;
                        UINT chars = 0;
                        if (VerQueryValueW(block.data(), query.c_str(), reinterpret_cast<LPVOID*>(&text), &chars) && text && chars > 0)
                        {
                            const std::string name = shown(std::wstring(text, wcsnlen(text, chars)));
                            if (!name.empty())
                            {
                                return name;
                            }
                        }
                    }
                }
                return {};
            }

            std::string fileName(const std::wstring& path)
            {
                const size_t slash = path.find_last_of(L"\\/");
                return shown(slash == std::wstring::npos ? path : path.substr(slash + 1));
            }

            bool userCommand(const std::wstring& scheme, std::wstring& out)
            {
                bool expand = false;
                return readString(HKEY_CURRENT_USER, CLASSES + scheme + L"\\shell\\open\\command", nullptr, out, expand);
            }

            // Is HKCU's command for this scheme ours?
            bool oursInUser(const std::wstring& scheme, const std::wstring& command)
            {
                std::wstring current;
                return userCommand(scheme, current) && iequal(current, command);
            }

            // Is HKCU's registration still the one a Charis Viewer wrote over this backup (another
            // install, or this one before it moved)? Told by the command, not by a name a later
            // installer may leave behind (review middle 1): any other program that registered since
            // wrote another command.
            bool heldByCharis(const std::wstring& scheme, const std::wstring& backup)
            {
                std::wstring current;
                std::wstring owner;
                bool expand = false;
                return userCommand(scheme, current)
                       && readString(HKEY_CURRENT_USER, backup, OWNER_COMMAND, owner, expand)
                       && !owner.empty() && iequal(current, owner);
            }

            // Keeps what the five values are now (the string ones present, each with its type).
            // A value of another type is not one an installer writes there: kept as absent.
            bool saveBackup(const std::wstring& cls, const std::wstring& backup)
            {
                RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());   // an older backup (it is not ours any more)
                HKEY key = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, backup.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                {
                    return false;
                }
                bool ok         = true;
                bool long_value = false;
                for (const Slot& slot : SLOTS)
                {
                    Raw raw;
                    const Read read = readRaw(HKEY_CURRENT_USER, cls + slot.subkey, slot.value, raw);
                    if (read == Read::Error)
                    {
                        ok = false;
                        break;
                    }
                    if (read == Read::Absent || !isString(raw.type) || raw.data.size() > MAX_STRING_BYTES)
                    {
                        continue;
                    }
                    long_value = long_value || raw.data.size() > UNINSTALLER_MAX_BYTES;
                    const std::wstring type_name = std::wstring(slot.saved) + L"Type";
                    const DWORD type = raw.type;
                    ok = RegSetValueExW(key, slot.saved, 0, raw.type, raw.data.empty() ? nullptr : raw.data.data(),
                                        static_cast<DWORD>(raw.data.size())) == ERROR_SUCCESS
                         && RegSetValueExW(key, type_name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&type),
                                           sizeof(type)) == ERROR_SUCCESS;
                    if (!ok)
                    {
                        break;
                    }
                }
                if (ok && long_value)
                {
                    // NSIS strings stop at 1024 characters: the uninstaller leaves this scheme to "Undo".
                    const DWORD one = 1;
                    ok = RegSetValueExW(key, LONG_VALUE, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one),
                                        sizeof(one)) == ERROR_SUCCESS;
                }
                RegCloseKey(key);
                if (!ok)
                {
                    RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());
                }
                return ok;
            }

            // Puts the five values back as the backup has them (absent there, or not a string of a
            // sane length: removed), removes the keys left empty and the backup.
            bool restoreBackup(const std::wstring& cls, const std::wstring& backup)
            {
                bool ok = true;
                for (const Slot& slot : SLOTS)
                {
                    const std::wstring path = cls + slot.subkey;
                    Raw raw;
                    const Read read = readRaw(HKEY_CURRENT_USER, backup, slot.saved, raw);
                    if (read == Read::Found && isString(raw.type) && raw.data.size() <= MAX_STRING_BYTES)
                    {
                        ok = writeRaw(path, slot.value, raw.type, raw.data.empty() ? nullptr : raw.data.data(),
                                      static_cast<DWORD>(raw.data.size())) && ok;
                    }
                    else if (read != Read::Error)
                    {
                        ok = deleteValue(path, slot.value) && ok;
                    }
                    else
                    {
                        ok = false;
                    }
                }
                for (const wchar_t* subkey : SUBKEYS)
                {
                    deleteKeyIfEmpty(cls + subkey);
                }
                if (ok)
                {
                    RegDeleteTreeW(HKEY_CURRENT_USER, backup.c_str());
                    deleteKeyIfEmpty(BACKUP_ROOT);
                }
                return ok;
            }

            // took: this press kept a new backup (only then does a failure put the backup back).
            // over_charis: this press writes over another Charis Viewer's registration without a backup
            // of its own, so a failure cannot put that one back (review-slurl-handler-recheck low 2).
            // put_back: false when a failure's restore did not go through either.
            bool registerScheme(const std::wstring& scheme, const std::wstring& exe, const std::wstring& command,
                                bool& took, bool& over_charis, bool& put_back)
            {
                took        = false;
                over_charis = false;
                put_back    = true;
                const std::wstring cls    = CLASSES + scheme;
                const std::wstring backup = BACKUP_ROOT + L"\\" + scheme;
                // Pressed again while ours: the backup already holds what was there before us.
                // Still the registration of a Charis Viewer elsewhere, over its backup: that backup holds
                // what was there before any Charis, so it stays, and "Undo" goes back to the viewer of
                // before (not to the other Charis).
                const bool ours = oursInUser(scheme, command);
                over_charis     = !ours && heldByCharis(scheme, backup);
                const bool keep = ours || over_charis;
                if (!keep)
                {
                    if (!saveBackup(cls, backup))
                    {
                        LL_WARNS("SLURLHandler") << "could not keep the current registration of "
                                                 << ll_convert<std::string>(scheme) << ": nothing written" << LL_ENDL;
                        return false;
                    }
                    took = true;
                }
                // No FriendlyAppName: one left by an earlier program would name it in the browser's
                // "Open ...?" for our command, and one of ours would stay behind when an installer
                // registers over us (review middle 1). Without it Windows shows the exe's
                // FileDescription ("Charis Viewer"). The one there before is in the backup.
                const bool ok = writeString(cls, nullptr, DESCRIPTION)
                                && writeString(cls, L"URL Protocol", L"")
                                && writeString(cls + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\"")
                                && deleteValue(cls + L"\\shell\\open", L"FriendlyAppName")
                                && writeString(cls + L"\\shell\\open\\command", nullptr, command)
                                && writeString(backup, OWNER_COMMAND, command);
                if (!ok)
                {
                    LL_WARNS("SLURLHandler") << "writing the registration of " << ll_convert<std::string>(scheme)
                                             << " failed" << (took ? "; putting back what was there" : "") << LL_ENDL;
                    if (took)
                    {
                        put_back = restoreBackup(cls, backup);
                    }
                }
                return ok;
            }

            // The program Windows starts for a scheme: Windows' default apps first, then HKCU, then HKLM.
            struct Handler
            {
                std::wstring command;
                std::wstring friendly;
                bool         expand     = false;
                bool         userChoice = false;
                bool         perUser    = false;
            };

            bool findHandler(const std::wstring& scheme, Handler& h)
            {
                bool unused = false;
                std::wstring progid;
                if (readString(HKEY_CURRENT_USER, USER_CHOICE + scheme + L"\\UserChoice", L"ProgId", progid, unused)
                    && !progid.empty() && progid.size() < 256
                    && readString(HKEY_CLASSES_ROOT, progid + L"\\shell\\open\\command", nullptr, h.command, h.expand))
                {
                    h.userChoice = true;
                    readString(HKEY_CLASSES_ROOT, progid + L"\\shell\\open", L"FriendlyAppName", h.friendly, unused);
                    return true;
                }
                for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
                {
                    if (readString(root, CLASSES + scheme + L"\\shell\\open\\command", nullptr, h.command, h.expand))
                    {
                        h.perUser = root == HKEY_CURRENT_USER;
                        readString(root, CLASSES + scheme + L"\\shell\\open", L"FriendlyAppName", h.friendly, unused);
                        return true;
                    }
                }
                return false;
            }

            // Who the handler's exe is, and the name to show.
            void describe(const Handler& h, const std::wstring& ours, Owner& owner, std::string& name, std::string& exe_out)
            {
                owner = Owner::None;
                name.clear();
                exe_out.clear();
                if (h.command.find_first_not_of(L" \t") == std::wstring::npos)
                {
                    return;
                }
                const std::wstring raw_exe = exeOfCommand(h.command, h.expand);
                const std::wstring full    = fullPath(raw_exe);
                exe_out = ll_convert<std::string>(full.empty() ? raw_exe : full);
                // A FriendlyAppName of "Charis Viewer" is believed for a Charis exe only: an installer
                // that registers over an older Charis build leaves it there (review middle 1).
                const bool charis_friendly = h.friendly == FRIENDLY_NAME;
                if (full.empty())
                {
                    owner = Owner::Missing;
                    name  = fileName(raw_exe);
                    return;
                }
                if (!ours.empty() && iequal(full, ours))
                {
                    owner = Owner::This;
                    name  = OUR_NAME;
                    return;
                }
                if (remote(full))
                {
                    // Not read: a sleeping NAS would stop the viewer here (review low 6).
                    owner = Owner::Other;
                    name  = charis_friendly ? std::string() : shown(h.friendly);
                    if (name.empty())
                    {
                        name = fileName(full);
                    }
                    return;
                }
                const std::wstring exe = normalize(full);
                if (!ours.empty() && iequal(exe, ours))
                {
                    owner = Owner::This;
                    name  = OUR_NAME;
                    return;
                }
                exe_out = ll_convert<std::string>(exe);
                if (!exists(exe))
                {
                    owner = Owner::Missing;
                    name  = fileName(exe);
                    return;
                }
                const std::string version_name = versionName(exe);
                owner = version_name == OUR_NAME ? Owner::OtherCharis : Owner::Other;
                name  = (charis_friendly && owner != Owner::OtherCharis) ? std::string() : shown(h.friendly);
                if (name.empty())
                {
                    name = version_name;
                }
                if (name.empty())
                {
                    name = fileName(exe);
                }
            }
        }

        bool supported()
        {
            return true;
        }

        std::string ourExe()
        {
            return ll_convert<std::string>(ourExeW());
        }

        Status status()
        {
            Status st;
            const std::wstring ours = ourExeW();

            Handler main;
            if (findHandler(MAIN_SCHEME, main))
            {
                st.userChoice = main.userChoice;
                st.perUser    = main.perUser;
                describe(main, ours, st.owner, st.name, st.exe);
            }
            // The other scheme may belong to another viewer while secondlife does not (review low 8).
            for (const wchar_t* scheme : SCHEMES)
            {
                Handler other;
                if (std::wstring(scheme) == MAIN_SCHEME || !findHandler(scheme, other))
                {
                    continue;
                }
                std::string unused_exe;
                describe(other, ours, st.secondOwner, st.secondName, unused_exe);
            }
            for (const wchar_t* each : SCHEMES)
            {
                st.undoable = st.undoable || oursInUser(each, commandFor(ours));
            }
            return st;
        }

        Result registerHere()
        {
            const std::wstring exe = ourExeW();
            if (!safeForCommand(exe))
            {
                LL_WARNS("SLURLHandler") << "this exe's path cannot go into a command line safely: '"
                                         << ll_convert<std::string>(exe) << "'; nothing written" << LL_ENDL;
                return Result::BadExe;
            }
            const std::wstring command = commandFor(exe);
            std::vector<std::wstring> done;   // backups kept in this press: put back if a later scheme fails
            bool unkept = false;              // an earlier scheme was another Charis's, written over without a backup
            for (const wchar_t* scheme : SCHEMES)
            {
                bool took        = false;
                bool over_charis = false;
                bool put_back    = true;
                if (!registerScheme(scheme, exe, command, took, over_charis, put_back))
                {
                    for (const std::wstring& earlier : done)
                    {
                        put_back = restoreBackup(CLASSES + earlier, BACKUP_ROOT + L"\\" + earlier) && put_back;
                    }
                    // "What was there before was put back" holds only when every scheme this press
                    // changed went back.
                    return (put_back && !over_charis && !unkept) ? Result::Failed : Result::Partial;
                }
                if (took)
                {
                    done.emplace_back(scheme);
                }
                unkept = unkept || over_charis;
            }
            LL_INFOS("SLURLHandler") << "secondlife:// links now open " << ll_convert<std::string>(exe)
                                     << " for this user (HKCU)" << LL_ENDL;
            return Result::Done;
        }

        Result undo()
        {
            const std::wstring command = commandFor(ourExeW());
            bool any = false;
            bool ok  = true;
            for (const wchar_t* scheme : SCHEMES)
            {
                // Another viewer registered since: its registration and the backup stay as they are.
                if (!oursInUser(scheme, command))
                {
                    continue;
                }
                any = true;
                ok = restoreBackup(CLASSES + scheme, BACKUP_ROOT + L"\\" + scheme) && ok;
            }
            if (!any)
            {
                return Result::NothingToDo;
            }
            LL_INFOS("SLURLHandler") << "secondlife:// registration of this exe undone"
                                     << (ok ? "" : " (some values could not be put back)") << LL_ENDL;
            return ok ? Result::Done : Result::Failed;
        }

#else // not Windows: the links are the desktop's business (llappviewerlinux/macosx)

        bool supported()
        {
            return false;
        }

        std::string ourExe()
        {
            return {};
        }

        Status status()
        {
            return {};
        }

        Result registerHere()
        {
            return Result::Failed;
        }

        Result undo()
        {
            return Result::NothingToDo;
        }
#endif
    }
}
