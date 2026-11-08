/**
 * @file posfile.h
 * @brief A file kept open for positional reads and writes (the texture cache's index, header records and fast cache)
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

#ifndef PLUS_TEXCACHE_POSFILE_H
#define PLUS_TEXCACHE_POSFILE_H

#include "stdtypes.h"

#include <filesystem>
#include <string>

// research/306 section 6 (P3a). The texture cache opened, read or wrote and closed texture.entries, texture.cache and
// FastCache.cache for every request: 2 to 4 times a request, and about 1.5 ms each where the antivirus looks at every
// open (research/306 6.1, tools/cache-io-bench). Here the file is opened once and every read and write names its
// position (Windows: ReadFile / WriteFile with an OVERLAPPED offset on a synchronous handle): the calls do not rely on a
// shared file position, so threads need no lock for the position (the system still does one I/O of a handle at a time).
// Windows only; elsewhere open() says no and the caller's own way is used.
//
// A handle is opened once and closed with the object (or close()), never in between: a closed handle's value is
// reused by the system for another file, and a caller that still holds the object would write into it.
namespace plus::texcache
{
    class PosFile
    {
    public:
        enum Mode
        {
            WRITER_CREATE,      // reads and writes; the file is made if it is not there
            READER              // reads only; the file has to be there
        };
        // What read() and write() return besides a number of bytes.
        static constexpr S64 TROUBLE_HANDLE = -1;   // the handle is not good any more (or sharing / access went wrong): the caller drops P3a and does the same again its own way
        static constexpr S64 TROUBLE_OTHER = -2;    // a failure of the data (disk full, a bad block): the same failure as the caller's own way would have

        PosFile() = default;
        ~PosFile() { close(); }
        PosFile(const PosFile&) = delete;
        PosFile& operator=(const PosFile&) = delete;

        // Opens the file with GENERIC_READ (| GENERIC_WRITE), shared with everyone (read, write, delete) and not
        // inherited. false: it could not be opened, not on a fixed local NTFS volume that can unlink open files
        // (the others keep the caller's own way), or not Windows; why is a short text for the log.
        bool open(const std::filesystem::path& file, Mode mode, std::string& why);
        void close();
        bool isOpen() const { return mHandle != nullptr; }

        // Bytes read / written (0 or fewer than asked at the end of the file), or a TROUBLE_ value. Not at a negative offset.
        S64 read(U64 offset, void* buffer, S32 bytes) const;
        S64 write(U64 offset, const void* buffer, S32 bytes) const;

        // A file kept open must be unlinkable and its name usable again at once (the delete the standard library does
        // is a POSIX one that does not wait for the handles; a filter driver may refuse it and fall back to the old
        // delete, which leaves the name "pending delete" while a handle is open). Tried with a probe file in dir: made
        // and opened as the cache files are, deleted, checked gone, made again under the same name. false: no (why).
        static bool unlinkWhileOpenWorks(const std::filesystem::path& dir, std::string& why);

        // GetLastError of the last failed call of this thread (for the log)
        static U32 lastError();

    private:
        void* mHandle = nullptr;
    };
}

#endif
