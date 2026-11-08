/**
 * @file subtitleclient.h
 * @brief Subtitles (plus/subtitle, research/137): the voice tap and the recognition helper.
 *
 * Tap: llwebrtc hands over a copy of the voice being played; it is converted to 16 kHz mono and kept
 * in a ring. Client: starts charis-subtitle.exe (tools/whisper-sub), feeds it the ring through a pipe
 * and turns the JSON lines it answers with into events. Protocol: tools/whisper-sub/README.md.
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

#ifndef PLUS_SUBTITLE_CLIENT_H
#define PLUS_SUBTITLE_CLIENT_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace plus
{
    namespace subtitle
    {
        // What the helper reported (one JSON line each).
        struct Event
        {
            enum class Type { Loading, Ready, Text, Dropped, Error, Exited };
            Type type = Type::Exited;
            double t0 = 0.0;       // Text: seconds of audio since start
            double t1 = 0.0;
            double seconds = 0.0;  // Dropped: how much audio was thrown away
            double computeMs = 0.0;
            std::string lang;      // Text
            std::string text;      // Text
            std::string message;   // Error
            // Text: where the sentence was heard. Set by Client::poll() (research/192).
            enum class Source { Voice, Stream };
            Source source = Source::Voice;
        };

        namespace tap
        {
            // Start / stop listening to the voice playout. Main thread.
            void start();
            void stop();
            // Reader side of the ring (one thread). Returns the number of samples copied.
            size_t read(int16_t* dst, size_t max);
            // Samples the ring had no room for (the helper fell behind) since the last start().
            size_t refused();
            // Samples taken into the ring since start(): the clock of the subtitles. The recognizer's
            // t0 / t1 are seconds on the same clock (samples / RATE) as long as nothing was refused.
            uint64_t producedSamples();
        }

        class Client
        {
        public:
            Client();
            ~Client();

            // Starts the helper. On failure returns false and says why in `error` (English; the
            // window shows its own text for the usual causes). threads >= 1; lang is "auto" or a code.
            // The tap starts listening once the helper says it is ready, so the clock starts then.
            // use_flags: cut sentences by setVoiced() instead of by loudness (research/137 section 6).
            bool start(const std::string& exe_path, const std::string& model_path, int threads, const std::string& lang,
                       bool use_flags, std::string& error);
            // Asks the helper to quit, waits at most 100 ms, then ends it (with its job). Never waits on the
            // helper beyond that, even a frozen one. Safe to call when not running.
            void stop();
            bool running() const;

            // Main thread: the next event, if any.
            bool poll(Event& out);

            // Main thread: is anybody speaking now, according to the voice server? Only passed on when
            // start() was given use_flags.
            void setVoiced(bool voiced);

            // Main thread: also write out the parcel's radio (streamtap, research/192). The voice goes first:
            // the radio is taken only while nobody has been heard for a moment. Each time the source
            // changes, the sentence being heard is closed. The radio's sentences are cut by loudness whatever
            // start() was given about use_flags (the voice server says nothing about the radio); the voice's by
            // the flags when use_flags.
            void setStreamEnabled(bool enabled);

            // Seconds of sound handed to the helper so far, voice and radio together: the clock the helper's
            // t0 / t1 are on. Starts at 0 when the helper is ready.
            double sentSeconds() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };

        // Where the model is looked for / saved (the folder is not created here).
        std::string modelFolder();
        // The helper next to the viewer (only there: not the model's folder, security audit A L1). name: its file name.
        std::string helperPath(const char* name = "charis-subtitle.exe");
    }
}

#endif // PLUS_SUBTITLE_CLIENT_H
