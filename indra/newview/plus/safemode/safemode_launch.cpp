/**
 * @file safemode_launch.cpp
 * @brief Safe mode (research/254): before anything of the viewer is made,
 *        a start that asks for safe mode runs the viewer again with the safe
 *        switches (safemode_table.h) and waits for it.
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

// Why a new process: the LLRHI_* switches are read by static initializers
// (llrhi.cpp and the others), which have all run before wWinMain. Setting the
// environment here would come too late for them; a new process reads the
// safe values from its first instruction, and no switch needs to know about
// safe mode. The first process only looks at its command line and the Shift
// key, starts the second and waits for it (its exit code is the viewer's),
// so scripts that wait for the viewer's process keep working. The second is
// in a job of the first's, so killing the first ends the second too.
//
// Nothing here may use the viewer's services: logging, the settings and
// gDirUtilp's app folders do not exist yet. Only the OS folders LLDir_Win32
// found when it was made (a static object of llfilesystem, made before
// wWinMain) are read, to find the two small files of safemode_files.h. What
// went wrong is kept for the log (relaunchError(), written by safemode.cpp
// once the log is up).
//
// A start is a safe start when it asks for one (--safe, Shift held), when
// the user turned on "start in safe mode next time too" (a file, as the
// settings are not read yet), or when the starts before did not get through
// (the starting marker counts OFFER_AFTER or more) and the user says yes to
// the question. The question is asked once for such a row: whatever the
// answer, the marker goes, so the next start counts from 0 again; a safe
// start does not count (safemode.cpp), so a safe mode that ends on the way
// does not ask again and again; one that went into safe mode by itself
// forgets the row. The GPU benchmark subprocess and the shader bake are never
// safe starts, and the unattended tools (ENV_NO_PROMPT) are never asked and
// do not follow "next time too".
//
// No text of other languages in this file (the compiler reads it in the
// system's code page): the Japanese question is written with \u escapes, its
// text is in research/254 9.

#include "llviewerprecompiledheaders.h"

#include "safemode.h"
#include "safemode_files.h"
#include "safemode_table.h"

#include "lldir.h"
#include "plus/independence/independence.h"
#include "plus/plus.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#include <shellapi.h>   // CommandLineToArgvW
#endif

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

std::string plus::safemode::files::userAppDirBeforeInit()
{
#if !LL_WINDOWS
    return std::string();
#else
    // As LLDir_Win32::initAppDirs() (its fall-back to the OS folder when the app folder
    // cannot be made is not followed: then the feature's init tells, earlyFilesFound()).
#if LL_VELOPACK
    // LLDir::isPortableInstall() and PORTABLE_USER_DATA_DIRNAME are protected: the same
    // test (a ".portable" file in the folder above the program's) and name, lldir.cpp.
    const std::string& exe_dir = gDirUtilp->getExecutableDir();
    const std::string install_root = exe_dir.empty() ? std::string() : gDirUtilp->getDirName(exe_dir);
    if (!install_root.empty())
    {
        const DWORD portable = GetFileAttributesW(ll_convert_string_to_wide(gDirUtilp->add(install_root, ".portable")).c_str());
        if (portable != INVALID_FILE_ATTRIBUTES && !(portable & FILE_ATTRIBUTE_DIRECTORY))
        {
            return gDirUtilp->add(install_root, "UserData");
        }
    }
#endif
    const std::string& os_dir = gDirUtilp->getOSUserDir();
    return os_dir.empty() ? std::string() : gDirUtilp->add(os_dir, plus::independence::APP_DIR_NAME);
#endif
}

#if LL_WINDOWS
namespace
{
    using namespace plus::safemode;

    constexpr wchar_t ARG_SAFE[] = L"--safe";
    constexpr wchar_t ARG_SAFE_SHORT[] = L"-safe";              // the parser takes long options with one dash too
    constexpr wchar_t ARG_GPU_BENCHMARK[] = L"--gpubenchmark";  // the viewer's own benchmark subprocess
    constexpr wchar_t ARG_BAKE_SHADERS[] = L"--bake-shaders";    // tools/bake-shaders.sh: never baked in safe mode
    constexpr DWORD REASON_CHARS = 32;

    // The question (MessageBoxW, %d: the count). Japanese for a Japanese Windows display language,
    // else English (the viewer's language setting is not read yet). The Japanese text is in research/254 9.
    constexpr wchar_t OFFER_TITLE[] = L"Charis Viewer";
    constexpr wchar_t OFFER_TEXT_EN[] =
        L"The last %d starts did not finish (the viewer closed before it finished starting).\n\n"
        L"Start in safe mode?\nSome new drawing features are turned off.\n\n"
        L"No: start as usual.";
    constexpr wchar_t OFFER_TEXT_JA[] =
        L"\u524d\u56de\u307e\u3067\u306e\u8d77\u52d5\u304c\u5b8c\u4e86\u3057\u3066\u3044\u307e\u305b\u3093"
        L"\uff08%d \u56de\u7d9a\u3051\u3066\u3001\u9014\u4e2d\u3067\u7d42\u308f\u308a\u307e\u3057\u305f\uff09\u3002\n\n"
        L"\u30bb\u30fc\u30d5\u30e2\u30fc\u30c9\u3067\u8d77\u52d5\u3057\u307e\u3059\u304b\uff1f\n"
        L"\u4e00\u90e8\u306e\u65b0\u3057\u3044\u63cf\u753b\u6a5f\u80fd\u3092\u5207\u3063\u3066\u8d77\u52d5\u3057\u307e\u3059\u3002\n\n"
        L"\uff3b\u3044\u3044\u3048\uff3d\u3092\u62bc\u3059\u3068\u3001\u3044\u3064\u3082\u3069\u304a\u308a\u8d77\u52d5\u3057\u307e\u3059\u3002";

    DWORD sRelaunchError = 0;
    Question sQuestion;

    // The variable is set (to anything; an empty value too: the size it needs counts its terminator).
    bool environmentHas(const wchar_t* name)
    {
        return GetEnvironmentVariableW(name, nullptr, 0) > 0;
    }

    // ENV_MARK is set: this process is a safe start or was started by one. Never starts another.
    bool markPresent()
    {
        return environmentHas(ENV_MARK);
    }

    // This process's reason when ENV_MARK holds one of ours, else null.
    const wchar_t* startReason()
    {
        static const wchar_t* const reason = []() -> const wchar_t*
        {
            wchar_t value[REASON_CHARS] = {};
            const DWORD len = GetEnvironmentVariableW(ENV_MARK, value, REASON_CHARS);
            if (len == 0 || len >= REASON_CHARS)
            {
                return nullptr;
            }
            const wchar_t* const known_reasons[] = { REASON_ARGUMENT, REASON_SHIFT, REASON_SETTING, REASON_OFFER };
            for (const wchar_t* known : known_reasons)
            {
                if (!wcscmp(value, known))
                {
                    return known;
                }
            }
            return nullptr;
        }();
        return reason;
    }

    struct Arguments
    {
        bool safe = false;
        bool tool = false;      // the GPU benchmark subprocess or the shader bake
    };

    // Whole words of the command line only (a SLURL that merely contains "--safe" is not one).
    Arguments readArguments()
    {
        Arguments found;
        int count = 0;
        LPWSTR* words = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!words)
        {
            return found;
        }
        for (int i = 1; i < count; ++i)
        {
            found.safe |= !wcscmp(words[i], ARG_SAFE) || !wcscmp(words[i], ARG_SAFE_SHORT);
            found.tool |= !wcscmp(words[i], ARG_GPU_BENCHMARK) || !wcscmp(words[i], ARG_BAKE_SHADERS);
        }
        LocalFree(words);
        return found;
    }

    bool shiftHeld()
    {
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    // A file of safemode_files.h, where the viewer will have put it. Empty when the user folder cannot be told.
    std::wstring earlyPath(const char* folder, const char* file)
    {
        static const std::string user_dir = plus::safemode::files::userAppDirBeforeInit();    // once a start
        return user_dir.empty() ? std::wstring() : ll_convert_string_to_wide(gDirUtilp->add(user_dir, folder, file));
    }

    bool fileThere(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    // "Start in safe mode next time too" is on (its file; plus::safemode::nextStartSafe() is the
    // running viewer's copy, made later).
    bool nextStartFileThere()
    {
        return fileThere(earlyPath(plus::safemode::files::SETTINGS_FOLDER, plus::safemode::files::NEXT_START_SAFE));
    }

    // Another Charis Viewer is running (its named mutex, LLAppViewerWin32's create_app_mutex()): the
    // starting marker may be its own, still going on.
    bool anotherInstanceRunning()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, plus::independence::APP_MUTEX_NAME);
        if (!mutex)
        {
            return false;
        }
        CloseHandle(mutex);
        return true;
    }

    // The starts in a row before this one that did not get through: the count the starting marker
    // holds (safemode_files.h), 0 without one.
    int unfinishedCount(const std::wstring& marker)
    {
        if (!fileThere(marker))
        {
            return 0;
        }
        std::ifstream in(marker.c_str());
        return plus::safemode::files::readStartingCount(in);
    }

    bool japaneseDisplay()
    {
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
    }

    std::wstring markerPath()
    {
        return earlyPath(plus::safemode::files::LOGS_FOLDER, plus::safemode::files::STARTING_MARKER);
    }

    // Removes the marker. 0, or the Windows error when it is still there.
    DWORD removeMarker(const std::wstring& marker)
    {
        if (DeleteFileW(marker.c_str()))
        {
            return 0;
        }
        const DWORD error = GetLastError();
        return fileThere(marker) ? (error ? error : ERROR_ACCESS_DENIED) : 0;
    }

    // The count of the marker when it is a row to ask about (OFFER_AFTER or more) and its own (no other
    // Charis Viewer runs: the marker may be that one's, still going on), else 0.
    int unfinishedRow(const std::wstring& marker)
    {
        const int count = unfinishedCount(marker);
        return count >= plus::safemode::files::OFFER_AFTER && !anotherInstanceRunning() ? count : 0;
    }

    // When the starts before did not get through OFFER_AFTER times in a row, asks whether this start
    // is to be a safe one (sQuestion keeps what happened, for the log). Asked once for such a row: the
    // marker goes first, whatever the answer will be (a safe start does not write one; a normal one
    // counts from 1 again). When it cannot be removed, nothing is asked (else every start would ask
    // again). True: yes.
    bool userWantsSafeStart()
    {
        const std::wstring marker = markerPath();
        const int count = unfinishedRow(marker);
        if (!count)
        {
            return false;
        }
        sQuestion.after = count;
        sQuestion.error = removeMarker(marker);
        if (sQuestion.error)
        {
            return false;
        }
        wchar_t text[512] = {};
        swprintf(text, std::size(text), japaneseDisplay() ? OFFER_TEXT_JA : OFFER_TEXT_EN, count);
        const int answer = MessageBoxW(nullptr, text, OFFER_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1 | MB_SETFOREGROUND);
        sQuestion.asked = true;
        sQuestion.yes = answer == IDYES;
        return sQuestion.yes;
    }

    // A start that went into safe mode by itself (--safe, Shift, next time too) forgets a row of starts
    // that did not get through: the next normal start does not ask about them.
    void forgetUnfinishedRow()
    {
        const std::wstring marker = markerPath();
        if (unfinishedRow(marker))
        {
            removeMarker(marker);   // still there: the next normal start asks, as before
        }
    }

    // This start's command line with "--set <name> <value>" for each of SETTINGS right after the
    // program's name, ahead of the user's words (the parser ignores what comes after a SLURL).
    // The program's name ends where the C runtime ends it for wWinMain's lpCmdLine, which the new
    // process's parser reads (the UCRT's rule, research/275 L072): at the first character up to a
    // blank (control characters too) outside quotes, a quote anywhere in it opening or closing them.
    // False when a quote is left open (research/275 L073): "--set" would go into the program's name,
    // so there is no safe start (this start goes on as a normal one).
    bool childCommandLine(std::wstring& child)
    {
        const wchar_t* const line = GetCommandLineW();
        const wchar_t* rest = line;
        bool in_quotes = false;
        while (*rest > L' ' || (*rest && in_quotes))
        {
            if (*rest == L'"')
            {
                in_quotes = !in_quotes;
            }
            ++rest;
        }
        if (in_quotes)
        {
            return false;
        }
        child.assign(line, rest);
        for (const Switch& setting : SETTINGS)
        {
            child += L" --set ";
            child += setting.name;
            child += L' ';
            child += setting.value;
        }
        child += rest;      // from the blank that ended the name (or nothing)
        return true;
    }

    // Sets this process's environment variables for the new process to inherit, and puts back
    // what was there when it goes (whether the new process started or not: a start that goes on
    // as a normal one must not read the safe values later).
    class EnvironmentScope
    {
    public:
        ~EnvironmentScope()
        {
            for (auto it = mSaved.rbegin(); it != mSaved.rend(); ++it)
            {
                SetEnvironmentVariableW(it->name, it->had ? it->value.c_str() : nullptr);
            }
        }

        bool set(const wchar_t* name, const wchar_t* value)
        {
            Saved saved{ name };
            const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);   // with the terminator; 0: not set
            if (size > 0)
            {
                std::vector<wchar_t> old(size);
                const DWORD len = GetEnvironmentVariableW(name, old.data(), size);
                saved.had = len < size;     // an empty value too (len 0)
                if (saved.had)
                {
                    saved.value.assign(old.data(), len);
                }
            }
            mSaved.push_back(std::move(saved));
            return SetEnvironmentVariableW(name, value) != 0;
        }

    private:
        struct Saved
        {
            const wchar_t* name;
            bool had = false;
            std::wstring value;
        };
        std::vector<Saved> mSaved;
    };

    // The window state this process was started with (a shortcut's "Run: Maximized"), for the new one,
    // and the shortcut it was started from (STARTF_TITLEISLINKNAME: lpTitle is the .lnk's path; or an
    // AppUserModelID), so the taskbar groups and pins the safe start's window as that shortcut's
    // (research/275 L064). lpTitle points into this process's start-up block, which lasts as long as it.
    STARTUPINFOW childStartupInfo()
    {
        STARTUPINFOW parent = {};
        parent.cb = sizeof(parent);
        GetStartupInfoW(&parent);
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        if (parent.dwFlags & STARTF_USESHOWWINDOW)
        {
            startup.dwFlags |= STARTF_USESHOWWINDOW;
            startup.wShowWindow = parent.wShowWindow;
        }
        constexpr DWORD SHORTCUT_FLAGS = STARTF_TITLEISLINKNAME | STARTF_TITLEISAPPID | STARTF_PREVENTPINNING;
        if ((parent.dwFlags & (STARTF_TITLEISLINKNAME | STARTF_TITLEISAPPID)) && parent.lpTitle && *parent.lpTitle)
        {
            startup.dwFlags |= parent.dwFlags & SHORTCUT_FLAGS;
            startup.lpTitle = parent.lpTitle;
        }
        return startup;
    }

    // The job the safe start runs in: when this process ends (killed too), its handle goes and the safe
    // start ends with it, as a viewer whose process a script kills would. Processes the safe start
    // starts itself (media plugins, a browser, Explorer) are not in it (SILENT_BREAKAWAY_OK) and live
    // on as they would. Null when it cannot be made (then the safe start just runs without one).
    HANDLE sChildJob = nullptr;

    HANDLE jobEndingWithThisProcess(HANDLE child)
    {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
        {
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, child))
        {
            CloseHandle(job);
            return nullptr;
        }
        return job;
    }

    void closeChildJob()
    {
        if (sChildJob)
        {
            CloseHandle(sChildJob);
            sChildJob = nullptr;
        }
    }

    // Starts this program again as a safe start (the switches of ENV_SWITCHES and SETTINGS, ENV_MARK
    // = reason). The Windows error when it could not be started, else 0 (process holds its handles).
    DWORD startSafeProcess(const wchar_t* reason, PROCESS_INFORMATION& process)
    {
        const std::wstring program = plus::ownExePath();   // not looked up on PATH or in the current folder
        if (program.empty())
        {
            return GetLastError() ? GetLastError() : ERROR_BAD_PATHNAME;
        }
        std::wstring command;
        if (!childCommandLine(command))
        {
            return ERROR_BAD_ARGUMENTS;
        }
        std::vector<wchar_t> command_buffer(command.begin(), command.end());   // CreateProcessW may write to it
        command_buffer.push_back(L'\0');

        EnvironmentScope environment;
        for (const Switch& entry : ENV_SWITCHES)
        {
            if (!environment.set(entry.name, entry.value))
            {
                return GetLastError();
            }
        }
        if (!environment.set(ENV_MARK, reason))
        {
            return GetLastError();
        }
        STARTUPINFOW startup = childStartupInfo();
        // No handles inherited; the environment is this process's, as set above. Suspended until it is in
        // the job (it runs nothing of its own before that).
        if (!CreateProcessW(program.c_str(), command_buffer.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                            nullptr, &startup, &process))
        {
            return GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
        }
        sChildJob = jobEndingWithThisProcess(process.hProcess);
        if (ResumeThread(process.hThread) == (DWORD)-1)
        {
            const DWORD error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            closeChildJob();
            return error;
        }
        return 0;
    }

    // Waits for the process to end and lets go of it. Its exit code (0 when it cannot be read).
    int waitForExit(PROCESS_INFORMATION& process)
    {
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        if (!GetExitCodeProcess(process.hProcess, &code))
        {
            code = 0;
        }
        CloseHandle(process.hProcess);
        closeChildJob();    // it has ended: nothing left in the job
        return (int)code;
    }
}

bool plus::safemode::relaunchIfAsked(int& exit_code)
{
    if (markPresent())
    {
        return false;   // a safe start (or started by one): never another
    }
    const Arguments arguments = readArguments();
    if (arguments.tool)
    {
        return false;   // started by the viewer for its GPU benchmark, or by tools/bake-shaders.sh
    }
    const wchar_t* reason = arguments.safe          ? REASON_ARGUMENT
                            : shiftHeld()           ? REASON_SHIFT
                            : unattended()          ? nullptr       // a tool's start: no question, no next-time file
                            : nextStartFileThere()  ? REASON_SETTING
                            : userWantsSafeStart()  ? REASON_OFFER
                                                    : nullptr;
    if (!reason)
    {
        return false;
    }
    if (reason != REASON_OFFER)
    {
        forgetUnfinishedRow();
    }
    PROCESS_INFORMATION process = {};
    sRelaunchError = startSafeProcess(reason, process);
    if (sRelaunchError)
    {
        return false;   // this start goes on as a normal one; safemode.cpp logs the error
    }
    // At a Windows shutdown, after the safe start (processes are told from the highest level down;
    // the default is 0x280): this one only waits, and ending first would end the safe start with the
    // job before it has saved anything.
    SetProcessShutdownParameters(0x100, 0);
    exit_code = waitForExit(process);
    return true;
}

bool plus::safemode::active()
{
    return startReason() != nullptr;
}

bool plus::safemode::unattended()
{
    static const bool set = environmentHas(ENV_NO_PROMPT);
    return set;
}

bool plus::safemode::inJobEndingWithFirst()
{
    // The job this process is in (null: its own); for a safe start that is the first process's
    // (the innermost one, assigned last).
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
    {
        return false;   // in no job
    }
    return (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}

const char* plus::safemode::reason()
{
    // The reasons are plain ASCII words (safemode_table.h).
    static const std::string name = []()
    {
        std::string ascii;
        for (const wchar_t* why = startReason(); why && *why; ++why)
        {
            ascii += (char)*why;
        }
        return ascii;
    }();
    return name.c_str();
}

unsigned long plus::safemode::relaunchError()
{
    return sRelaunchError;
}

const plus::safemode::Question& plus::safemode::question()
{
    return sQuestion;
}

void plus::safemode::forgetSafeEnvironment()
{
    // The values this process was started with stay where its code reads them: std::getenv reads
    // the C runtime's copy of the environment, made at the start, which SetEnvironmentVariableW
    // does not change. Only the block new processes get is cleared.
    active();   // the reason is kept (startReason()'s static) before ENV_MARK goes
    for (const Switch& entry : ENV_SWITCHES)
    {
        SetEnvironmentVariableW(entry.name, nullptr);
    }
    SetEnvironmentVariableW(ENV_MARK, nullptr);
}

#else // LL_WINDOWS

bool plus::safemode::relaunchIfAsked(int&)
{
    return false;
}

bool plus::safemode::active()
{
    return false;
}

bool plus::safemode::unattended()
{
    return false;
}

bool plus::safemode::inJobEndingWithFirst()
{
    return false;
}

const char* plus::safemode::reason()
{
    return "";
}

unsigned long plus::safemode::relaunchError()
{
    return 0;
}

const plus::safemode::Question& plus::safemode::question()
{
    static const Question none;
    return none;
}

void plus::safemode::forgetSafeEnvironment()
{
}

#endif // LL_WINDOWS
