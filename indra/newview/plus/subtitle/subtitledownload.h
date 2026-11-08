/**
 * @file subtitledownload.h
 * @brief Subtitles (plus/subtitle, research/137 section 4): fetching the recognition model.
 *
 * The model (ggml-small.bin, about 490 MB) is not shipped. The window offers a button; pressing it is
 * the user's go-ahead. The file comes from Hugging Face with the curl.exe that Windows ships (the
 * viewer's own HTTP code keeps a reply in memory, which is no way to take 490 MB), goes to a ".part"
 * file, is checked for its size and SHA-256, and only then gets its name. Nothing else is downloaded.
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

#ifndef PLUS_SUBTITLE_DOWNLOAD_H
#define PLUS_SUBTITLE_DOWNLOAD_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace plus
{
    namespace subtitle
    {
        // What to fetch, and what it must be.
        struct ModelSource
        {
            std::string url;
            std::string fileName;     // in modelFolder()
            uint64_t size = 0;        // bytes
            std::string sha256;       // lower case hex
        };

        // An address a download may be started with: https only, not longer than 2048, and only the characters an address is
        // made of. A quote, a space, a backslash or a control character is refused: the address goes onto curl's command
        // line, and a quote could end its argument and begin another (security audit C, L2).
        inline bool validDownloadUrl(const std::string& url)
        {
            if (url.size() < 9 || url.size() > 2048 || url.compare(0, 8, "https://") != 0)
            {
                return false;
            }
            static const char sPunctuation[] = "-._~:/?#[]@!$&'()*+,;=%";
            for (const unsigned char c : url)
            {
                const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
                if (!plain && !(c != 0 && c < 0x80 && std::strchr(sPunctuation, c)))
                {
                    return false;
                }
            }
            return true;
        }

        // The model the window offers (research/137: small; base can be added the same way).
        const ModelSource& smallModel();

        class ModelDownload
        {
        public:
            enum class State { Idle, Running, Verifying, Done, Failed };

            ModelDownload();
            ~ModelDownload();   // cancels

            // Main thread. Returns false (and says why) when it cannot start; the state stays Idle.
            bool start(const ModelSource& source, const std::string& folder, std::string& error);
            // Stops a download in progress and removes the partial file. Safe at any time.
            void cancel();

            State state() const;
            // 0..1 while Running (the size of the partial file against the expected size).
            double fraction() const;
            // Why it is Failed (English, for the log; the window has its own words).
            std::string failure() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };
    }
}

#endif // PLUS_SUBTITLE_DOWNLOAD_H
