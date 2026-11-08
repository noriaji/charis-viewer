/**
 * @file ttsengine.h
 * @brief The speech engine behind the chat read-aloud (plus/tts): the interface and its engines.
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

// This header (and ttsengine_winrt.cpp) includes nothing of the viewer, so that the Windows Runtime
// headers of the engine do not meet the viewer's macros. A second engine (VOICEVOX, over HTTP) goes in
// beside the WinRT one: implement Engine and return it from createEngine() in tts.cpp.

#ifndef PLUS_TTSENGINE_H
#define PLUS_TTSENGINE_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace plus::tts
{
    enum class VoiceGender
    {
        Neutral,
        Male,
        Female
    };

    // One line to say. Everything but the volume (Engine::setVolume, so that a change reaches the lines
    // already waiting) is in the call.
    struct Utterance
    {
        std::string text;                       // UTF-8, already cleaned (tts.cpp)
        VoiceGender gender = VoiceGender::Neutral;
        float       pitch  = 1.0f;              // 1 = the voice's own; 0.5 .. 1.5 is used
        float       rate   = 1.0f;              // 1 = normal speed
        std::string preferredMale;             // name of the voice to use for a man, "" = choose
        std::string preferredFemale;            // ... for a woman
        std::string speaker;                    // who says it (an id as text), "" = nobody (notices, samples): dropSpeaker()
    };

    struct VoiceInfo
    {
        std::string name;                       // as the system shows it ("Microsoft Haruka")
        std::string language;                   // "ja-JP", "en-US", ...
        VoiceGender gender = VoiceGender::Neutral;
    };

    // Speaks on a thread of its own: speak() and clear() only touch a queue and return at once, so the
    // frame is never held by the speech.
    class Engine
    {
    public:
        virtual ~Engine() = default;

        // The engine's short name, for the log.
        virtual const char* name() const = 0;
        // True once the engine is started and could make a voice; false when it failed to start.
        virtual bool ready() const = 0;
        // True when the engine has given up starting (no voice will come). ready() and failed() are both
        // false while it starts.
        virtual bool failed() const = 0;
        // Queue a line. When maxQueue lines wait already, the oldest waiting ones are dropped (not the
        // one being spoken).
        virtual void speak(Utterance utterance, std::size_t maxQueue) = 0;
        // Drop what waits and stop what is spoken now.
        virtual void clear() = 0;
        // Drop the waiting lines of one speaker (Utterance::speaker, not "") and stop the line spoken now
        // when it is that speaker's (the worker stops it within a few milliseconds).
        virtual void dropSpeaker(const std::string& speaker) = 0;
        // The volume (0 .. 1) for what is said from now on, also the lines already waiting: it is read
        // when a piece of a line is made into sound, so a change reaches the next piece.
        virtual void setVolume(float volume) = 0;
        // How many lines wait (the one being spoken is not counted).
        virtual std::size_t pending() const = 0;
        // The voices the system has; empty until the engine has started.
        virtual std::vector<VoiceInfo> voices() const = 0;
        // Stop the thread. After this the engine does nothing.
        virtual void shutdown() = 0;
    };

    // The Windows speech synthesizer (Windows.Media.SpeechSynthesis); ttsengine_winrt.cpp.
    std::unique_ptr<Engine> createWinRTEngine();

    // One line for the viewer's log; tts.cpp. Callable from any thread.
    void logLine(const std::string& line);
}

#endif // PLUS_TTSENGINE_H
