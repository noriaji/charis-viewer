/**
 * @file posfile.cpp
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

#include "linden_common.h"

#include "posfile.h"

#if LL_WINDOWS
#include <windows.h>
#endif

namespace plus::texcache
{
#if LL_WINDOWS
namespace
{
    thread_local DWORD tLastError = 0;

    // A trouble of the handle, not of the data: the caller does the same again its own way.
    bool handleTrouble(DWORD error)
    {
        switch (error)
        {
        case ERROR_INVALID_HANDLE:
        case ERROR_FILE_INVALID:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_NETNAME_DELETED:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_UNEXP_NET_ERR:
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_INVALID_PARAMETER:
            return true;
        default:
            return false;
        }
    }

#ifndef FILE_SUPPORTS_POSIX_UNLINK_RENAME
#define FILE_SUPPORTS_POSIX_UNLINK_RENAME 0x00000400
#endif
}

bool PosFile::open(const std::filesystem::path& file, Mode mode, std::string& why)
{
    close();
    const DWORD access = mode == WRITER_CREATE ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    const DWORD disposition = mode == WRITER_CREATE ? OPEN_ALWAYS : OPEN_EXISTING;
    // not inherited (the default: no SECURITY_ATTRIBUTES); shared with everyone: other viewers, the antivirus, a cache
    // that is cleared meanwhile
    HANDLE handle = CreateFileW(file.native().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        tLastError = GetLastError();
        why = "CreateFileW failed, error " + std::to_string(tLastError);
        return false;
    }
    // the volume: a fixed local disk, NTFS, where an open file can be unlinked (else the caller's own way)
    DWORD flags = 0;
    wchar_t fs_name[MAX_PATH + 1] = {};
    if (!GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, &flags, fs_name, MAX_PATH))
    {
        tLastError = GetLastError();
        why = "GetVolumeInformationByHandleW failed, error " + std::to_string(tLastError);
        CloseHandle(handle);
        return false;
    }
    // NTFS only (ReFS / Dev Drive and the others keep the old way: not tried), where an open file can be unlinked
    if (_wcsicmp(fs_name, L"NTFS") != 0 || !(flags & FILE_SUPPORTS_POSIX_UNLINK_RENAME))
    {
        why = "the volume is not NTFS that unlinks open files (NTFS of Windows 10 1709 and later)";
        CloseHandle(handle);
        return false;
    }
    wchar_t root[MAX_PATH + 1] = {};
    std::error_code absolute_ec;
    std::filesystem::path absolute = std::filesystem::absolute(file, absolute_ec);
    if (absolute_ec || !absolute.has_root_path() || GetVolumePathNameW(absolute.native().c_str(), root, MAX_PATH) == 0 || GetDriveTypeW(root) != DRIVE_FIXED)
    {
        why = "not on a fixed local drive";
        CloseHandle(handle);
        return false;
    }
    mHandle = handle;
    return true;
}

bool PosFile::unlinkWhileOpenWorks(const std::filesystem::path& dir, std::string& why)
{
    std::error_code ec;
    // probe files left by an earlier start that could not delete its own are cleared first
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::wstring name = it->path().filename().wstring();
        if (name.compare(0, 14, L"posfile-probe-") == 0)
        {
            std::error_code remove_ec;
            std::filesystem::remove(it->path(), remove_ec);
        }
    }
    ec.clear();
    const std::filesystem::path probe = dir / (L"posfile-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE handle = CreateFileW(probe.native().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        why = "the probe file could not be made, error " + std::to_string(GetLastError());
        return false;
    }
    std::error_code remove_ec, exists_ec;
    std::filesystem::remove(probe, remove_ec);
    // gone: the delete worked and looking for the name says it is not there (an error of the look, as the access
    // denied of a name that is "pending delete", is not "gone")
    const bool gone = !remove_ec && !std::filesystem::exists(probe, exists_ec) && !exists_ec;
    bool again = false;
    if (gone)
    {
        // the name is usable at once while the handle is still open
        HANDLE second = CreateFileW(probe.native().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (second != INVALID_HANDLE_VALUE)
        {
            again = true;
            CloseHandle(second);
        }
    }
    CloseHandle(handle);
    std::filesystem::remove(probe, ec);     // whatever is left (a probe that stays is cleared at the next start)
    if (!gone || !again)
    {
        why = gone ? "a deleted file's name cannot be made again while a handle is open (the delete waits for the handles)"
                   : "a file with a handle open is not unlinked by the delete (it waits for the handles)";
        if (remove_ec || exists_ec)
        {
            why += ", error " + std::to_string((remove_ec ? remove_ec : exists_ec).value());
        }
        return false;
    }
    return true;
}

void PosFile::close()
{
    if (mHandle)
    {
        CloseHandle(static_cast<HANDLE>(mHandle));
        mHandle = nullptr;
    }
}

S64 PosFile::read(U64 offset, void* buffer, S32 bytes) const
{
    if (!mHandle || !buffer || bytes < 0)
    {
        return TROUBLE_HANDLE;
    }
    if (bytes == 0)
    {
        return 0;
    }
    OVERLAPPED overlapped = {};     // zeroed: no event, only the position
    overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    if (!ReadFile(static_cast<HANDLE>(mHandle), buffer, static_cast<DWORD>(bytes), &done, &overlapped))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF)
        {
            return done;    // at the end of the file: the bytes there were
        }
        tLastError = error;
        return handleTrouble(error) ? TROUBLE_HANDLE : TROUBLE_OTHER;
    }
    return done;
}

S64 PosFile::write(U64 offset, const void* buffer, S32 bytes) const
{
    if (!mHandle || !buffer || bytes < 0)
    {
        return TROUBLE_HANDLE;
    }
    if (bytes == 0)
    {
        return 0;
    }
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    if (!WriteFile(static_cast<HANDLE>(mHandle), buffer, static_cast<DWORD>(bytes), &done, &overlapped))
    {
        const DWORD error = GetLastError();
        tLastError = error;
        return handleTrouble(error) ? TROUBLE_HANDLE : TROUBLE_OTHER;
    }
    return done;
}

U32 PosFile::lastError()
{
    return tLastError;
}

#else   // not Windows: the caller's own way

bool PosFile::open(const std::filesystem::path&, Mode, std::string& why)
{
    why = "positional kept-open files are for Windows only";
    return false;
}
bool PosFile::unlinkWhileOpenWorks(const std::filesystem::path&, std::string& why)
{
    why = "positional kept-open files are for Windows only";
    return false;
}
void PosFile::close() {}
S64 PosFile::read(U64, void*, S32) const { return TROUBLE_HANDLE; }
S64 PosFile::write(U64, const void*, S32) const { return TROUBLE_HANDLE; }
U32 PosFile::lastError() { return 0; }

#endif
}
