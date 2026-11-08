/**
 * @file subtitledownload.cpp
 * @brief Subtitles (plus/subtitle, research/137 section 4): fetching the recognition model.
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

#include "subtitledownload.h"
#include "helperprocess.h"
#include "subtitlehash.h"

#include "lldir.h"
#include "llfile.h"
#include "llstring.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#if LL_WINDOWS
#include <windows.h>
#endif

namespace plus
{
    namespace subtitle
    {
        const ModelSource& smallModel()
        {
            // size and SHA-256 are the ones Hugging Face lists for the file (checked 2026-10-21)
            static const ModelSource sSmall = {
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin",
                "ggml-small.bin",
                487601967ULL,
                "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b",
            };
            return sSmall;
        }

#if LL_WINDOWS

        namespace
        {
            // How long a cancelled curl is given to go (it takes milliseconds). cancel() waits for this on the
            // main thread, so it stays short; a .part that curl still held is removed by the next start().
            constexpr DWORD CURL_END_MS = 200;

            uint64_t fileSize(const std::wstring& path)
            {
                WIN32_FILE_ATTRIBUTE_DATA a;
                if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
                {
                    return 0;
                }
                return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
            }
        }

        struct ModelDownload::Impl
        {
            std::atomic<int> state{ int(State::Idle) };
            std::atomic<bool> cancelled{ false };
            HANDLE process = nullptr;
            HANDLE job = nullptr;    // curl's job: closing it ends curl, so a viewer that goes away takes curl along
            std::thread worker;
            std::wstring part;       // the partial file
            std::wstring dest;       // the finished file
            std::string partUtf8;
            ModelSource source;

            mutable std::mutex mutex;
            std::string failure;

            void fail(const std::string& why)
            {
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    failure = why;
                }
                DeleteFileW(part.c_str());
                state = int(State::Failed);
            }

            // Set by cancel() so that the worker stops waiting for curl at once (manual reset; start() resets it).
            HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);

            ~Impl()
            {
                if (job)
                {
                    CloseHandle(job);
                }
                if (wake)
                {
                    CloseHandle(wake);
                }
            }

            // Runs on the worker thread: waits for curl, then checks and names the file.
            void run()
            {
                // this thread owns the handle; cancel() only sets the flag and the event
                const HANDLE waits[2] = { process, wake };
                bool overrun = false;   // the .part grew beyond the size the file is to have: a server that does not stop
                for (;;)
                {
                    const DWORD r = WaitForMultipleObjects(wake ? 2 : 1, waits, FALSE, 200);
                    if (r == WAIT_OBJECT_0)
                    {
                        break;   // curl ended
                    }
                    overrun = (r == WAIT_TIMEOUT) && fileSize(part) > source.size;
                    if (cancelled || overrun || r == WAIT_FAILED)
                    {
                        // WAIT_FAILED cannot happen with good handles; if it does, curl is not left writing
                        endProcessTree(process, job);   // curl and anything it started
                        WaitForSingleObject(process, CURL_END_MS);   // ends at once; one that hangs is not waited for longer
                        break;
                    }
                }
                DWORD code = 1;
                GetExitCodeProcess(process, &code);
                CloseHandle(process);
                process = nullptr;
                if (job)
                {
                    CloseHandle(job);
                    job = nullptr;
                }
                if (cancelled)
                {
                    DeleteFileW(part.c_str());
                    state = int(State::Idle);
                    return;
                }
                if (overrun)
                {
                    fail("the download is larger than the file is expected to be");
                    return;
                }
                if (code != 0)
                {
                    fail("curl.exe stopped with code " + std::to_string(code));
                    return;
                }
                state = int(State::Verifying);
                if (fileSize(part) != source.size)
                {
                    fail("the downloaded file has the wrong size");
                    return;
                }
                // 490 MB take seconds to read: the hash looks at the flag before every piece
                const std::string sha = sha256File(partUtf8, &cancelled);
                if (cancelled)
                {
                    DeleteFileW(part.c_str());
                    state = int(State::Idle);
                    return;
                }
                if (sha != source.sha256)
                {
                    fail("the downloaded file does not match its SHA-256");
                    return;
                }
                if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
                {
                    fail("cannot name the file (error " + std::to_string(GetLastError()) + ")");
                    return;
                }
                state = int(State::Done);
            }
        };

        ModelDownload::ModelDownload() : mImpl(new Impl) {}

        ModelDownload::~ModelDownload()
        {
            cancel();
        }

        bool ModelDownload::start(const ModelSource& source, const std::string& folder, std::string& error)
        {
            Impl& m = *mImpl;
            const State now = state();
            if (now == State::Running || now == State::Verifying)
            {
                error = "already downloading";
                return false;
            }
            if (m.worker.joinable())
            {
                m.worker.join();
            }
            wchar_t sysdir[MAX_PATH];
            if (!GetSystemDirectoryW(sysdir, MAX_PATH))
            {
                error = "cannot find the Windows folder";
                return false;
            }
            const std::wstring curl = std::wstring(sysdir) + L"\\curl.exe";
            if (GetFileAttributesW(curl.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                error = "curl.exe is not part of this Windows";
                return false;
            }
            // The address goes onto curl's command line: an https address of plain address characters only (no quote that
            // could end the argument and start another), checked here for every download.
            if (!validDownloadUrl(source.url))
            {
                error = "the download address is not an https address of the allowed form";
                return false;
            }
            // The whole path, not one level: the model folder is <user>\subtitle\translate, and mkdir alone fails
            // (errno 2) while "subtitle" is missing, which then made curl exit 23 (cannot write the .part file).
            std::error_code folderError;
            std::filesystem::create_directories(std::filesystem::path(ll_convert_string_to_wide(folder)), folderError);
            if (!LLFile::isdir(folder))
            {
                error = "cannot create the folder for the download";
                return false;
            }
            m.source = source;
            m.dest = ll_convert_string_to_wide(folder + gDirUtilp->getDirDelimiter() + source.fileName);
            m.partUtf8 = folder + gDirUtilp->getDirDelimiter() + source.fileName + ".part";
            m.part = ll_convert_string_to_wide(m.partUtf8);
            DeleteFileW(m.part.c_str());

            // -L follows the redirect to the file server (HTTPS only, there too), --fail turns an HTTP error
            // into an exit code. A connection that is not made in 30 s, or that brings less than 4 KB/s for
            // a minute, is given up and tried again (--retry counts it as a passing error); without these a
            // stalled transfer would wait forever with the window saying "downloading".
            //
            // -q first: curl then reads no .curlrc (a "proxy" or "insecure" there is not ours to obey); -g: no URL
            // globbing ([ ] in an address); --max-filesize: a server that announces more than the file is expected
            // to be is refused before it starts (run() also stops a .part that grows beyond it). Security audit C.
            std::wstring cmd = L"\"" + curl + L"\" -q -g -L --proto =https --proto-redir =https --fail --silent --show-error"
                               L" --connect-timeout 30 --speed-limit 4096 --speed-time 60 --retry 3 --max-filesize " +
                               std::to_wstring(source.size) + L" -o \"" + m.part + L"\" \"" + ll_convert_string_to_wide(source.url) + L"\"";
            std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
            cmdline.push_back(L'\0');
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            // The program is named on its own too: no search, exactly the file checked above.
            if (!CreateProcessW(curl.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                nullptr, &si, &pi))
            {
                error = "cannot start curl.exe (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            // Tie curl to the viewer, as the helpers are (helperprocess.h): when the viewer goes away,
            // so does curl, instead of writing on in the background.
            m.job = tieToViewer(pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            m.process = pi.hProcess;
            m.cancelled = false;
            if (m.wake)
            {
                ResetEvent(m.wake);
            }
            m.state = int(State::Running);
            m.worker = std::thread([&m] { m.run(); });
            return true;
        }

        void ModelDownload::cancel()
        {
            Impl& m = *mImpl;
            m.cancelled = true;
            if (m.wake)
            {
                SetEvent(m.wake);
            }
            if (m.worker.joinable())
            {
                // Short: the event wakes the worker, which ends curl (it goes at once), and a SHA-256 in
                // progress stops before its next 1 MiB piece.
                m.worker.join();
            }
            if (state() == State::Running || state() == State::Verifying)
            {
                m.state = int(State::Idle);
            }
        }

        ModelDownload::State ModelDownload::state() const
        {
            return State(mImpl->state.load());
        }

        double ModelDownload::fraction() const
        {
            const Impl& m = *mImpl;
            if (state() != State::Running || m.source.size == 0)
            {
                return 0.0;
            }
            const double f = double(fileSize(m.part)) / double(m.source.size);
            return f > 1.0 ? 1.0 : f;
        }

        std::string ModelDownload::failure() const
        {
            std::lock_guard<std::mutex> lk(mImpl->mutex);
            return mImpl->failure;
        }

#else   // the model download is Windows only (curl.exe of Windows)

        struct ModelDownload::Impl {};
        ModelDownload::ModelDownload() : mImpl(new Impl) {}
        ModelDownload::~ModelDownload() {}
        bool ModelDownload::start(const ModelSource&, const std::string&, std::string& error)
        {
            error = "the model download is available on Windows only";
            return false;
        }
        void ModelDownload::cancel() {}
        ModelDownload::State ModelDownload::state() const { return State::Idle; }
        double ModelDownload::fraction() const { return 0.0; }
        std::string ModelDownload::failure() const { return std::string(); }

#endif
    }
}
