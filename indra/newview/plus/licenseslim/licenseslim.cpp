/**
 * @file licenseslim.cpp
 * @brief The start-up check of D3DCompiler_47.dll, which the release no longer
 *        ships (research/268).
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

// CEF's copies of Microsoft's d3dcompiler_47.dll and dxil.dll are left out of the
// distribution (viewer_manifest.py; they are redistributables without source, which the
// SignPath Foundation's terms do not allow). ANGLE in the CEF processes loads
// D3DCompiler_47.dll by name, so with no copy next to dullahan_host.exe the standard DLL
// search order takes the one in System32, part of Windows 10 and later. This writes one log
// line at start-up with that file's version, or a warning when System32 has none (Web pages
// and media may then fail to draw). Once, at start-up; nothing per frame.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#if LL_WINDOWS
#include "lldir.h"
#include "llfile.h"
#include "llstring.h"
#include "llwin32headers.h"

#include <vector>

#pragma comment(lib, "version.lib")   // GetFileVersionInfoW, VerQueryValueW
#endif

namespace plus
{
    namespace licenseslim
    {
        namespace
        {
#if LL_WINDOWS
            constexpr char DLL_NAME[] = "D3DCompiler_47.dll";

            // "10.0.26100.4654" from the file's fixed version block, empty when it has none.
            std::string fileVersion(const std::wstring& path)
            {
                DWORD handle = 0;
                const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
                if (size == 0)
                {
                    return {};
                }
                std::vector<BYTE> block(size);
                if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data()))
                {
                    return {};
                }
                VS_FIXEDFILEINFO* info = nullptr;
                UINT length = 0;
                if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length)
                    || !info || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE)
                {
                    return {};
                }
                return llformat("%u.%u.%u.%u",
                                (U32)HIWORD(info->dwFileVersionMS), (U32)LOWORD(info->dwFileVersionMS),
                                (U32)HIWORD(info->dwFileVersionLS), (U32)LOWORD(info->dwFileVersionLS));
            }

            void init()
            {
                // A copy next to dullahan_host.exe is loaded before the system's: one an older
                // build left in its build folder (the release has none).
                const std::string bundled = gDirUtilp->getLLPluginDir() + gDirUtilp->getDirDelimiter() + DLL_NAME;
                if (LLFile::isfile(bundled))
                {
                    LL_INFOS("Plus") << "licenseslim: " << bundled << " is next to the Web plugin and is loaded "
                                     << "before the system's (left by an older build; the release has none)" << LL_ENDL;
                }

                wchar_t system_dir[MAX_PATH] = {};
                const UINT chars = GetSystemDirectoryW(system_dir, MAX_PATH);
                if (chars == 0 || chars >= MAX_PATH)
                {
                    LL_WARNS("Plus") << "licenseslim: the system folder is not known; "
                                     << DLL_NAME << " not checked" << LL_ENDL;
                    return;
                }
                const std::wstring path = std::wstring(system_dir, chars) + L"\\" + ll_convert_string_to_wide(DLL_NAME);
                const std::string path_utf8 = ll_convert_wide_to_string(path);
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    LL_WARNS("Plus") << "licenseslim: no " << path_utf8 << " (the viewer ships no copy of it): "
                                     << "Web pages, the login screen and media on prims may not show" << LL_ENDL;
                    return;
                }
                const std::string version = fileVersion(path);
                LL_INFOS("Plus") << "licenseslim: " << path_utf8 << " version "
                                 << (version.empty() ? std::string("unknown") : version)
                                 << " (for the Web plugin; the viewer ships no copy of it)" << LL_ENDL;
            }

            plus::FeatureRegistration sRegistration({ "licenseslim", &init, {} });
#endif
        }
    }
}
