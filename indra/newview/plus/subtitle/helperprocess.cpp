/**
 * @file helperprocess.cpp
 * @brief Subtitles (plus/subtitle, research/273 #4): starting a helper program on two pipes, tied to the viewer.
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

#include "helperprocess.h"

#if LL_WINDOWS

#include "llstring.h"

#include <vector>

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            void closeHandle(HANDLE& h)
            {
                if (h)
                {
                    CloseHandle(h);
                    h = nullptr;
                }
            }
        }

        bool HelperProcess::start(const std::string& exe_path, const std::wstring& args, DWORD in_pipe_size, DWORD out_pipe_size,
                                  std::string& error)
        {
            SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };   // the helper's ends are inherited
            HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
            if (!CreatePipe(&inRead, &inWrite, &sa, in_pipe_size) || !CreatePipe(&outRead, &outWrite, &sa, out_pipe_size))
            {
                for (HANDLE h : { inRead, inWrite, outRead, outWrite })
                {
                    if (h)
                    {
                        CloseHandle(h);
                    }
                }
                error = "cannot create the pipes";
                return false;
            }
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);    // ours are not
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

            const std::wstring app = ll_convert_string_to_wide(exe_path);
            const std::wstring cmd = L"\"" + app + L"\" " + args;
            // Only the helper's two pipe ends and our stderr are inherited, not every handle the viewer has.
            // A handle in the list must be inheritable (else CreateProcess fails, error 87): our
            // stderr goes in only when it is one (not, for one, without a console or redirection).
            HANDLE inherit[3] = { inRead, outWrite, GetStdHandle(STD_ERROR_HANDLE) };
            DWORD  errFlags   = 0;
            const bool errInheritable = inherit[2] && inherit[2] != INVALID_HANDLE_VALUE &&
                                        GetHandleInformation(inherit[2], &errFlags) && (errFlags & HANDLE_FLAG_INHERIT);
            DWORD inheritCount = errInheritable ? 3 : 2;
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            std::vector<char> attrBuf(attrSize);
            LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
            bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                           UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inheritCount * sizeof(HANDLE),
                                                     nullptr, nullptr);
            STARTUPINFOEXW si = {};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = inRead;
            si.StartupInfo.hStdOutput = outWrite;
            si.StartupInfo.hStdError = inheritCount == 3 ? inherit[2] : nullptr;
            si.lpAttributeList = attrs;
            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
            cmdline.push_back(L'\0');
            // the program is named on its own too: no search, exactly the file that the caller checked
            const BOOL created = attrsOk && CreateProcessW(app.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                                           CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                                           nullptr, &si.StartupInfo, &pi);
            if (attrsOk)
            {
                DeleteProcThreadAttributeList(attrs);
            }
            CloseHandle(inRead);
            CloseHandle(outWrite);
            if (!created)
            {
                error = "cannot start " + exe_path + " (error " + std::to_string(GetLastError()) + ")";
                CloseHandle(inWrite);
                CloseHandle(outRead);
                return false;
            }

            // Tie the helper to the viewer: when the viewer goes away, so does it.
            job = tieToViewer(pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);

            process = pi.hProcess;
            toHelper = inWrite;
            fromHelper = outRead;
            return true;
        }

        void HelperProcess::endWithin(DWORD grace_ms)
        {
            if (process && WaitForSingleObject(process, grace_ms) != WAIT_OBJECT_0)
            {
                endProcessTree(process, job);
            }
        }

        void HelperProcess::close()
        {
            closeHandle(toHelper);
            closeHandle(fromHelper);
            closeHandle(process);
            closeHandle(job);
        }

        HANDLE tieToViewer(HANDLE process)
        {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
                AssignProcessToJobObject(job, process);
            }
            return job;
        }

        void endProcessTree(HANDLE process, HANDLE job)
        {
            if (job)
            {
                TerminateJobObject(job, 1);   // the process and anything it started
            }
            TerminateProcess(process, 1);     // also when it could not be put in the job
        }

        bool writeAll(HANDLE pipe, const void* data, size_t len)
        {
            const char* p = static_cast<const char*>(data);
            while (len)
            {
                DWORD done = 0;
                if (!WriteFile(pipe, p, (DWORD)len, &done, nullptr) || done == 0)
                {
                    return false;
                }
                p += done;
                len -= done;
            }
            return true;
        }

        void readLines(HANDLE pipe, size_t max_pending, const std::function<void(const std::string&)>& on_line)
        {
            std::string pending;
            char chunk[4096];
            for (;;)
            {
                DWORD got = 0;
                if (!ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
                {
                    break;   // the helper closed its output (it exited)
                }
                pending.append(chunk, got);
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    on_line(line);
                }
                if (pending.size() > max_pending)
                {
                    // A helper that writes on without a line end: what it wrote is no line we know (the rest of it
                    // up to the next line end is given to on_line, which does not know it either).
                    pending.clear();
                }
            }
        }
    }
}

#endif // LL_WINDOWS
