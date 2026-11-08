/**
 * @file subtitlehash.h
 * @brief Subtitles (plus/subtitle, research/137): SHA-256 of a file or of bytes, with the Windows
 *        crypto API (advapi32, which every Windows program already links). Used to check the model
 *        that was downloaded. No viewer dependencies: subtitle_test.cpp builds this on its own.
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

#ifndef PLUS_SUBTITLE_HASH_H
#define PLUS_SUBTITLE_HASH_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace plus
{
    namespace subtitle
    {
        // Lower case hex of the SHA-256, or an empty string if the crypto API is not there.
        class Sha256
        {
        public:
            Sha256()
            {
#ifdef _WIN32
                if (CryptAcquireContextW(&mProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                    CryptCreateHash(mProv, CALG_SHA_256, 0, 0, &mHash))
                {
                    mOk = true;
                }
#endif
            }
            ~Sha256()
            {
#ifdef _WIN32
                if (mHash)
                {
                    CryptDestroyHash(mHash);
                }
                if (mProv)
                {
                    CryptReleaseContext(mProv, 0);
                }
#endif
            }
            Sha256(const Sha256&) = delete;
            Sha256& operator=(const Sha256&) = delete;

            void add(const void* data, size_t len)
            {
#ifdef _WIN32
                if (mOk && len)
                {
                    mOk = CryptHashData(mHash, static_cast<const BYTE*>(data), (DWORD)len, 0) != FALSE;
                }
#else
                (void)data;
                (void)len;
#endif
            }

            std::string hex()
            {
#ifdef _WIN32
                if (!mOk)
                {
                    return std::string();
                }
                BYTE digest[32];
                DWORD n = sizeof(digest);
                if (!CryptGetHashParam(mHash, HP_HASHVAL, digest, &n, 0) || n != 32)
                {
                    return std::string();
                }
                static const char* d = "0123456789abcdef";
                std::string out;
                for (BYTE b : digest)
                {
                    out += d[b >> 4];
                    out += d[b & 15];
                }
                return out;
#else
                return std::string();
#endif
            }

        private:
#ifdef _WIN32
            HCRYPTPROV mProv = 0;
            HCRYPTHASH mHash = 0;
#endif
            bool mOk = false;
        };

        // Reads the file in pieces. Empty string if it cannot be read, or if `stop` (when given) turns
        // true: it is looked at before every piece, so a cancel is seen within one piece (1 MiB).
        inline std::string sha256File(const std::string& path, const std::atomic<bool>* stop = nullptr)
        {
            if (stop && stop->load())
            {
                return std::string();
            }
            FILE* f = nullptr;
#ifdef _WIN32
            // the path is UTF-8
            int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n : 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
            f = _wfopen(w.c_str(), L"rb");
#else
            f = fopen(path.c_str(), "rb");
#endif
            if (!f)
            {
                return std::string();
            }
            Sha256 h;
            std::vector<unsigned char> buf(1 << 20);
            bool stopped = false;
            for (;;)
            {
                if (stop && stop->load())
                {
                    stopped = true;
                    break;
                }
                const size_t got = fread(buf.data(), 1, buf.size(), f);
                if (got == 0)
                {
                    break;
                }
                h.add(buf.data(), got);
            }
            const bool error = ferror(f) != 0;
            fclose(f);
            return (error || stopped) ? std::string() : h.hex();
        }
    }
}

#endif // PLUS_SUBTITLE_HASH_H
