/**
 * @file translateclient.h
 * @brief Subtitles (plus/subtitle, research/217): the local translation helper, charis-translate.exe.
 *
 * The helper (tools/translate-helper) reads one JSON line per sentence on its stdin and answers one JSON line each
 * on its stdout. The viewer starts it when the first sentence is to be translated on this PC, and stops it when the
 * window closes. Nothing is sent to any service. Like charis-subtitle.exe it is tied to the viewer with a job
 * object, so it goes when the viewer goes.
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

#ifndef PLUS_SUBTITLE_TRANSLATECLIENT_H
#define PLUS_SUBTITLE_TRANSLATECLIENT_H

#include <cstdint>
#include <memory>
#include <string>

namespace plus
{
    namespace subtitle
    {
        class TranslateClient
        {
        public:
            struct Result
            {
                uint64_t id = 0;
                bool ok = false;
                std::string text;      // the translation
                std::string message;   // why not, when !ok (English, for the log)
            };

            TranslateClient();
            ~TranslateClient();   // stops

            // Starts the helper. models is the folder that holds en-ja and ja-en. False (and why) when it cannot.
            bool start(const std::string& exe_path, const std::string& models, int threads, std::string& error);
            // Ends the helper at once (it keeps nothing). Safe when not running.
            void stop();
            // The helper is there and has not exited.
            bool running() const;

            // Main thread. Queues one sentence; the answer comes through poll(). False when the helper is not running.
            bool request(uint64_t id, const std::string& from, const std::string& to, const std::string& text);
            // Main thread: the next answer, if any. A sentence the helper could not do comes back with ok false.
            bool poll(Result& out);

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };
    }
}

#endif // PLUS_SUBTITLE_TRANSLATECLIENT_H
