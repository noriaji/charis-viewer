/**
 * @file helper_lineio.h
 * @brief One JSON object per line on stdin and stdout: the line reader and writer of the Charis Viewer helpers.
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

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace charis_helper
{
    enum class LineRead
    {
        Line,      // line holds one line, without its '\n' (a "\r" before it is left for the caller)
        TooLong,   // the line had more than maxBytes bytes: it was read to its end and dropped (line is empty)
        End        // stdin is closed and nothing was left
    };

    // Reads up to the next '\n' or the end of the input. maxBytes 0 = no cap. A last line without its '\n' is
    // still a Line; the call after it is End.
    inline LineRead readLine(std::FILE* in, std::string& line, size_t maxBytes)
    {
        line.clear();
        bool tooLong = false;
        int  c;
        while ((c = std::fgetc(in)) != EOF && c != '\n')
        {
            if (tooLong || (maxBytes != 0 && line.size() >= maxBytes))
            {
                tooLong = true;   // keep reading to the end of the line, keep nothing
                continue;
            }
            line += char(c);
        }
        if (tooLong)
        {
            line.clear();
            return LineRead::TooLong;
        }
        if (c == EOF && line.empty())
        {
            return LineRead::End;
        }
        return LineRead::Line;
    }

    // One answer line on stdout, flushed at once (the viewer reads line by line).
    inline void emitLine(const std::string& line)
    {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    // No "\r\n" translation on the pipes.
    inline void setBinaryStdio()
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }
}
