/**
 * @file helperprocess.h
 * @brief Subtitles (plus/subtitle, research/273 #4): starting a helper program on two pipes, tied to the viewer.
 *
 * The recognition helper (charis-subtitle.exe, subtitleclient.cpp) and the translation helper
 * (charis-translate.exe, translateclient.cpp) are started the same way: their standard input and output on
 * pipes of their own, nothing else of the viewer's inherited but our stderr, no window, and in a job that ends
 * them when the viewer goes (even if it crashes). curl.exe for the model download (subtitledownload.cpp) has no
 * pipes but is tied to the viewer by the same job. What is said on the pipes is each client's own.
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

#ifndef PLUS_SUBTITLE_HELPERPROCESS_H
#define PLUS_SUBTITLE_HELPERPROCESS_H

#if LL_WINDOWS

#include <cstddef>
#include <functional>
#include <string>

#include <windows.h>

namespace plus
{
    namespace subtitle
    {
        // A helper program and our ends of its pipes. Not copied; the client that owns it closes it (close()).
        struct HelperProcess
        {
            HelperProcess() = default;
            HelperProcess(const HelperProcess&) = delete;
            HelperProcess& operator=(const HelperProcess&) = delete;

            HANDLE process    = nullptr;
            HANDLE job        = nullptr;   // the helper dies with the viewer, even if the viewer crashes
            HANDLE toHelper   = nullptr;   // its standard input
            HANDLE fromHelper = nullptr;   // its standard output

            // Starts exe_path, exactly that file (no search: the caller has checked it, helperPath()), with the command
            // line "<exe_path>" args. args is put in as it is: the caller quotes what needs it. in_pipe_size and
            // out_pipe_size are the pipes' buffer sizes. The helper starts once it is in the job. On failure nothing is
            // left open and error says why ("cannot create the pipes", "cannot start <exe> (error N)").
            bool start(const std::string& exe_path, const std::wstring& args, DWORD in_pipe_size, DWORD out_pipe_size,
                       std::string& error);

            // Gives the helper grace_ms to end by itself (it was told to quit), then ends it and anything it started.
            void endWithin(DWORD grace_ms);

            // Closes our ends of the pipes, the process and the job, in that order (the job last: closing it ends
            // what is still in it).
            void close();
        };

        // A job that ends the process (and anything it starts) when the job's last handle is closed: when the viewer
        // goes away, so does the process. The process must not have run yet (CREATE_SUSPENDED), so that nothing it
        // starts is outside the job. nullptr when no job could be made (the process then is not tied).
        HANDLE tieToViewer(HANDLE process);

        // Ends the process and anything it started (the job's), also when it could not be put in a job.
        void endProcessTree(HANDLE process, HANDLE job);

        // Writes all of data to the pipe; false when the other end is gone.
        bool writeAll(HANDLE pipe, const void* data, size_t len);

        // Reads the pipe until its other end closes, giving each line (without its "\n" or "\r\n") to on_line. What
        // has no line end yet is kept up to max_pending bytes; beyond that it is dropped (a helper that writes on
        // without a line end says nothing we know).
        void readLines(HANDLE pipe, size_t max_pending, const std::function<void(const std::string&)>& on_line);
    }
}

#endif // LL_WINDOWS

#endif // PLUS_SUBTITLE_HELPERPROCESS_H
