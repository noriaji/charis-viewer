/**
 * @file exitwatch.cpp
 * @brief Exit watchdog (research/292): see exitwatch.h.
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

#include "plus/fastexit/exitwatch.h"

#include "llviewercontrol.h"
#include "llversioninfo.h"
#include "llprofiler.h"
#include "lldir.h"

#include <atomic>
#include <string>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <tlhelp32.h>
#endif

namespace plus::exitwatch
{
#if LL_WINDOWS
namespace
{
    // State table: research/292 (research/149 R).
    // Written by prepare()/arm() on the main thread before the watchdog thread exists; the threads only read.
    // Everything the threads touch is leaked on purpose (they may run while the statics are destroyed).
    bool sEnabled = false;
    U32 sSeconds = 10;
    std::atomic<bool> sArmed{false};
    std::atomic<DWORD> sWatchTid{0};
    std::atomic<bool> sHold{false};     // the watchdog thread found a debugger / Tracy: report only
    const std::wstring* sLogPath = nullptr;
    const std::string* sHead = nullptr; // "<version> pid <n>" for the first line of a report
    ULONGLONG sSelfCreated = 0;         // this process's creation time, to tell children from reused PIDs

    using NtQueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    // Looked up by arm() on the main thread: the watchdog must not call GetModuleHandle/GetProcAddress
    // (they take the loader lock, which the stuck main thread may hold).
    NtQueryFn sNtQuery = nullptr;
    DescFn sGetDesc = nullptr;

    constexpr DWORD MAX_LOG_BYTES = 64 * 1024;
    constexpr DWORD REPORT_WAIT_MS = 3000;
    constexpr int MAX_THREAD_LINES = 64;

    ULONGLONG to_u64(const FILETIME& f)
    {
        return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
    }

    // The file name part of a path, UTF-8 (no folders: they hold the user's name).
    std::string base_name(const WCHAR* path)
    {
        const WCHAR* p = wcsrchr(path, L'\\');
        p = p ? p + 1 : path;
        char buf[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, p, -1, buf, (int)sizeof(buf) - 1, nullptr, nullptr);
        return buf;
    }

    // "module+0xoffset" of an address (file name only). Takes the loader lock: only from a thread that may block.
    std::string where_is(const void* addr)
    {
        HMODULE mod = nullptr;
        WCHAR path[MAX_PATH] = {};
        if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)addr, &mod)
            && GetModuleFileNameW(mod, path, MAX_PATH))
        {
            char off[32];
            snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)((const char*)addr - (const char*)mod));
            return base_name(path) + off;
        }
        return "?";
    }

    // A child of this process that was born after it (a reused PID of an older, dead parent does not count).
    bool is_real_child(const PROCESSENTRY32W& pe)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h)
        {
            return false;
        }
        FILETIME created, exited, kernel, user;
        const bool ok = GetProcessTimes(h, &created, &exited, &kernel, &user) && to_u64(created) >= sSelfCreated;
        CloseHandle(h);
        return ok;
    }

    struct Counts
    {
        int threads = 0;
        int children = 0;
    };

    // Walks the thread and process lists once. The calling thread and the watchdog thread are not counted.
    // When `out` is set, one line per thread (up to MAX_THREAD_LINES, with start address and name) and per
    // child process is appended.
    Counts survey(std::string* out, const char* eol)
    {
        Counts c;
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        const DWORD watch_tid = sWatchTid.load();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD | TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            return c;
        }
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == watch_tid)
            {
                continue;
            }
            ++c.threads;
            if (!out || c.threads > MAX_THREAD_LINES)
            {
                continue;
            }
            std::string line = "thread " + std::to_string(te.th32ThreadID);
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
            {
                th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            }
            if (th)
            {
                PVOID start = nullptr;
                if (sNtQuery && sNtQuery(th, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) == 0)
                {
                    line += " start " + where_is(start);
                }
                PWSTR name = nullptr;
                if (sGetDesc && SUCCEEDED(sGetDesc(th, &name)) && name)
                {
                    if (*name)
                    {
                        line += " name " + base_name(name);
                    }
                    LocalFree(name);
                }
                CloseHandle(th);
            }
            *out += line + eol;
        }
        if (out && c.threads > MAX_THREAD_LINES)
        {
            *out += "(" + std::to_string(c.threads - MAX_THREAD_LINES) + " more threads not listed)" + eol;
        }
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        {
            if (pe.th32ParentProcessID != pid || pe.th32ProcessID == pid || !is_real_child(pe))
            {
                continue;
            }
            ++c.children;
            if (out)
            {
                *out += "child " + std::to_string(pe.th32ProcessID) + " " + base_name(pe.szExeFile) + eol;
            }
        }
        CloseHandle(snap);
        return c;
    }

    // Plain Win32 on purpose: no LL logging (the log may be closed by now), no LL singletons.
    // The file is started again when it is over MAX_LOG_BYTES. A folder we cannot write to: nothing is written.
    void write_report(const std::string& text)
    {
        if (!sLogPath)
        {
            return;
        }
        HANDLE f = CreateFileW(sLogPath->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            return;
        }
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(f, &size) && size.QuadPart > MAX_LOG_BYTES)
        {
            SetEndOfFile(f);    // the pointer is at 0: the file is emptied
        }
        else
        {
            SetFilePointer(f, 0, nullptr, FILE_END);
        }
        DWORD written = 0;
        WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr);
        FlushFileBuffers(f);
        CloseHandle(f);
    }

    // The reporter: may block on the loader lock (GetModuleHandleEx, GetModuleFileName). The watchdog
    // thread waits for it with a limit and ends the process either way.
    DWORD WINAPI report(LPVOID)
    {
        std::string body;
        const Counts c = survey(&body, "\r\n");
        SYSTEMTIME t;
        GetLocalTime(&t);
        char head[320];
        snprintf(head, sizeof(head), "%04u-%02u-%02u %02u:%02u:%02u exit watchdog: %s; still alive %u s after the cleanup (threads %d, children %d)%s\r\n",
                 (unsigned)t.wYear, (unsigned)t.wMonth, (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond,
                 sHead ? sHead->c_str() : "?", (unsigned)sSeconds, c.threads, c.children,
                 sHold.load() ? "; a debugger or Tracy is attached, so the process is left alone" : "; ending it");
        write_report(std::string(head) + body);
        OutputDebugStringA(head);
        return 0;
    }

    // Only API calls that do not take the loader lock (Sleep, CreateThread, Wait, TerminateProcess,
    // IsDebuggerPresent): a main thread stuck inside the loader cannot hold this thread back.
    DWORD WINAPI watch(LPVOID)
    {
        Sleep(sSeconds * 1000);
        bool hold = IsDebuggerPresent() != FALSE;
#ifdef TracyIsConnected
        hold = hold || TracyIsConnected;
#endif
        sHold.store(hold);
        if (HANDLE rep = CreateThread(nullptr, 0, report, nullptr, 0, nullptr))
        {
            WaitForSingleObject(rep, REPORT_WAIT_MS);
            CloseHandle(rep);
        }
        if (!hold)
        {
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;
    }
}
#endif // LL_WINDOWS

void prepare()
{
#if LL_WINDOWS
    sEnabled = gSavedSettings.getBOOL("PlusExitWatchdog");
    sSeconds = llclamp(gSavedSettings.getU32("PlusExitWatchdogSeconds"), 3U, 120U);
    if (sEnabled && !sHead)
    {
        // Here, not in arm(): by then LLSingletonBase::deleteAll() has run, and LLVersionInfo::instance()
        // would build a new one (and a new LLEventPumps) from the default channel.
        sHead = new std::string(LLVersionInfo::instance().getChannelAndVersion() + " pid " + std::to_string(GetCurrentProcessId()));
        sLogPath = new std::wstring(ll_convert<std::wstring>(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "exit-watchdog.log")));
    }
#endif
}

void snapshot(const char* where, bool list_threads)
{
#if LL_WINDOWS
    if (!sEnabled)
    {
        return;
    }
    std::string body;
    const Counts c = survey(list_threads ? &body : nullptr, "\n");
    LL_INFOS("Plus") << "ExitWatch [" << where << "] other threads alive: " << c.threads << ", child processes: " << c.children
                     << (body.empty() ? "" : "\n") << body << LL_ENDL;
#endif
}

void arm()
{
#if LL_WINDOWS
    if (!sEnabled || sArmed.exchange(true))
    {
        return;
    }
    // Everything the threads need is made here (or in prepare()), on the main thread.
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        sNtQuery = (NtQueryFn)GetProcAddress(nt, "NtQueryInformationThread");
    }
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
    {
        sGetDesc = (DescFn)GetProcAddress(k32, "GetThreadDescription");
    }
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        sSelfCreated = to_u64(created);
    }
    snapshot("cleanup done", false);
    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, watch, nullptr, 0, &tid);
    if (th)
    {
        sWatchTid.store(tid);
        CloseHandle(th);
    }
    else
    {
        LL_WARNS("Plus") << "ExitWatch: could not start the watchdog thread" << LL_ENDL;
    }
#endif
}

} // namespace plus::exitwatch
