/**
 * @file ttsengine_winrt.cpp
 * @brief The Windows speech synthesizer as the engine of the chat read-aloud (plus/tts).
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

// Built without the viewer's precompiled header (tts.cmake) and without any viewer header: C++/WinRT
// and the viewer's macros do not mix. One thread of its own does everything that waits: it makes the
// sound of a line with SpeechSynthesizer (a WAV in memory), then plays it with waveOut. Lines of
// Japanese and of other text are made apart, each with a voice of its language.

#include "ttsengine.h"

// C++/WinRT (cppwinrt of the 10.0.26100 SDK) links RoGetActivationFactory and RoOriginateLanguageException by name:
// they are in runtimeobject.lib. (Not WindowsApp.lib, the umbrella library: it clashes with kernel32.)
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "winmm.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plus::tts
{
    namespace
    {
        using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;
        namespace ss = winrt::Windows::Media::SpeechSynthesis;
        using winrt::Windows::Media::SpeechSynthesis::VoiceInformation;

        std::wstring widen(const std::string& utf8)
        {
            if (utf8.empty())
            {
                return std::wstring();
            }
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
            if (n > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
            }
            return out;
        }

        std::string narrow(const std::wstring& wide)
        {
            if (wide.empty())
            {
                return std::string();
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
            if (n > 0)
            {
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
            }
            return out;
        }

        // Japanese text: kana, ideographs, and the signs and full-width forms around them.
        bool isJapaneseChar(wchar_t c)
        {
            return (c >= 0x3040 && c <= 0x30FF)     // hiragana, katakana
                || (c >= 0x3400 && c <= 0x4DBF)     // CJK extension A
                || (c >= 0x4E00 && c <= 0x9FFF)     // CJK ideographs
                || (c >= 0xF900 && c <= 0xFAFF)     // compatibility ideographs
                || (c >= 0xFF66 && c <= 0xFF9F)     // half-width katakana
                || c == 0x30FC || c == 0x3005;
        }

        // Letters that decide a run's language. Digits, spaces and signs follow the run before.
        bool isLetter(wchar_t c)
        {
            if (isJapaneseChar(c))
            {
                return true;
            }
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x00C0;
        }

        struct Run
        {
            std::wstring text;
            bool japanese = false;
        };

        std::vector<Run> splitByLanguage(const std::wstring& text)
        {
            std::vector<Run> runs;
            bool current_japanese = false;
            bool have_run = false;
            for (wchar_t c : text)
            {
                bool japanese = current_japanese;
                if (isLetter(c) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF))
                {
                    japanese = isJapaneseChar(c) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF);
                }
                if (!have_run || japanese != current_japanese)
                {
                    // A run that holds nothing but signs and spaces joins the next one.
                    if (have_run && std::none_of(runs.back().text.begin(), runs.back().text.end(), isLetter))
                    {
                        runs.back().japanese = japanese;
                    }
                    else
                    {
                        runs.push_back(Run());
                        runs.back().japanese = japanese;
                    }
                    current_japanese = japanese;
                    have_run = true;
                }
                runs.back().text.push_back(c);
            }
            return runs;
        }

        // The format and the samples of a WAV in memory (pointers into it).
        struct WavParts
        {
            const uint8_t* format     = nullptr;
            size_t         formatSize = 0;
            const uint8_t* data       = nullptr;
            size_t         dataSize   = 0;
        };

        bool findWavParts(const std::vector<uint8_t>& wav, WavParts& parts)
        {
            if (wav.size() < 20 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
            {
                return false;
            }
            size_t pos = 12;
            while (pos + 8 <= wav.size())
            {
                uint32_t chunk_size = 0;
                std::memcpy(&chunk_size, wav.data() + pos + 4, 4);
                const size_t body = pos + 8;
                if (std::memcmp(wav.data() + pos, "fmt ", 4) == 0)
                {
                    if (chunk_size < 16 || chunk_size > wav.size() - body)
                    {
                        return false;
                    }
                    parts.format     = wav.data() + body;
                    parts.formatSize = chunk_size;
                }
                else if (std::memcmp(wav.data() + pos, "data", 4) == 0)
                {
                    // A streamed WAV may give a size of 0 or too much: the end of the buffer rules.
                    parts.data     = wav.data() + body;
                    parts.dataSize = (chunk_size == 0 || chunk_size > wav.size() - body) ? wav.size() - body : chunk_size;
                    return parts.format != nullptr;
                }
                pos = body + static_cast<size_t>(chunk_size) + (chunk_size & 1);
            }
            return false;
        }

        // 16-bit integer samples: WAVE_FORMAT_PCM, or WAVE_FORMAT_EXTENSIBLE whose SubFormat is PCM.
        bool isPcm16(const WavParts& parts)
        {
            uint16_t tag  = 0;
            uint16_t bits = 0;
            std::memcpy(&tag, parts.format, 2);
            std::memcpy(&bits, parts.format + 14, 2);
            if (bits != 16)
            {
                return false;
            }
            if (tag == 1)
            {
                return true;
            }
            // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71}, at byte 24 of the fmt chunk.
            static const uint8_t PCM_SUBFORMAT[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                       0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
            return tag == 0xFFFE && parts.formatSize >= 40 && std::memcmp(parts.format + 24, PCM_SUBFORMAT, 16) == 0;
        }

        // Scales the 16-bit samples of a PCM WAV in memory. Other formats stay as they are.
        void applyVolume(std::vector<uint8_t>& wav, float volume)
        {
            WavParts parts;
            if (volume >= 0.999f || !findWavParts(wav, parts) || !isPcm16(parts))
            {
                return;
            }
            uint8_t* samples = wav.data() + (parts.data - wav.data());
            const float gain = std::max(0.0f, volume);
            for (size_t i = 0; i + 1 < parts.dataSize; i += 2)
            {
                int16_t sample = 0;
                std::memcpy(&sample, samples + i, 2);
                const float scaled = std::clamp(static_cast<float>(sample) * gain, -32768.0f, 32767.0f);
                sample = static_cast<int16_t>(scaled);
                std::memcpy(samples + i, &sample, 2);
            }
        }

        class WinRTEngine final : public Engine
        {
        public:
            WinRTEngine() : mThread([this] { run(); }) {}

            ~WinRTEngine() override { shutdown(); }

            const char* name() const override { return "WinRT SpeechSynthesizer"; }

            bool ready() const override { return mReady.load(); }

            bool failed() const override { return mFailed.load(); }

            void speak(Utterance utterance, std::size_t maxQueue) override
            {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mQuit)
                    {
                        return;
                    }
                    mQueue.push_back(std::move(utterance));
                    while (mQueue.size() > std::max<std::size_t>(maxQueue, 1))
                    {
                        mQueue.pop_front();
                    }
                }
                mWake.notify_one();
            }

            // The worker sees the new generation within a few milliseconds and stops its sound itself
            // (playAndWait), so a stop never reaches the next line's sound.
            void clear() override
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueue.clear();
                ++mGeneration;
            }

            void dropSpeaker(const std::string& speaker) override
            {
                if (speaker.empty())
                {
                    return;
                }
                std::lock_guard<std::mutex> lock(mMutex);
                mQueue.erase(std::remove_if(mQueue.begin(), mQueue.end(),
                                            [&speaker](const Utterance& u) { return u.speaker == speaker; }),
                             mQueue.end());
                if (mSpeaking == speaker)
                {
                    ++mGeneration;      // only the line spoken now carries the old generation
                }
            }

            void setVolume(float volume) override
            {
                mVolume = std::clamp(volume, 0.0f, 1.0f);
            }

            std::size_t pending() const override
            {
                std::lock_guard<std::mutex> lock(mMutex);
                return mQueue.size();
            }

            std::vector<VoiceInfo> voices() const override
            {
                std::lock_guard<std::mutex> lock(mMutex);
                return mVoiceList;
            }

            void shutdown() override
            {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mQuit)
                    {
                        return;
                    }
                    mQuit = true;
                    mQueue.clear();
                    ++mGeneration;
                }
                mWake.notify_all();
                if (mThread.joinable())
                {
                    mThread.join();
                }
            }

        private:
            struct Voice
            {
                VoiceInformation info{ nullptr };
                VoiceInfo        summary;
                bool             japanese = false;
            };

            void run()
            {
                bool apartment = false;
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    apartment = true;
                    mSynth = SpeechSynthesizer();
                    loadVoices();
                    mReady = true;
                }
                catch (const winrt::hresult_error& e)
                {
                    logLine("WinRT speech failed to start: " + narrow(std::wstring(e.message().c_str())));
                    mReady = false;
                }
                catch (const std::exception& e)
                {
                    logLine(std::string("WinRT speech failed to start: ") + e.what());
                    mReady = false;
                }
                if (!mReady)
                {
                    mFailed = true;
                    releaseOnThisThread(apartment);
                    return;
                }

                for (;;)
                {
                    Utterance utterance;
                    unsigned  generation = 0;
                    {
                        std::unique_lock<std::mutex> lock(mMutex);
                        mWake.wait(lock, [this] { return mQuit || !mQueue.empty(); });
                        if (mQuit)
                        {
                            break;
                        }
                        utterance  = std::move(mQueue.front());
                        mQueue.pop_front();
                        generation = mGeneration;
                        mSpeaking  = utterance.speaker;
                    }
                    try
                    {
                        speakNow(utterance, generation);
                    }
                    catch (const winrt::hresult_error& e)
                    {
                        logLine("WinRT speech failed on a line: " + narrow(std::wstring(e.message().c_str())));
                    }
                    catch (const std::exception& e)
                    {
                        logLine(std::string("WinRT speech failed on a line: ") + e.what());
                    }
                    std::lock_guard<std::mutex> lock(mMutex);
                    mSpeaking.clear();
                }
                releaseOnThisThread(apartment);
            }

            // The WinRT objects (the synthesizer, the voices) are let go on the thread that made them,
            // before its apartment is closed; not later by the destructor on the main thread.
            void releaseOnThisThread(bool apartment)
            {
                mVoices.clear();
                mSynth = nullptr;
                if (apartment)
                {
                    winrt::uninit_apartment();
                }
            }

            void loadVoices()
            {
                std::vector<Voice> all;
                std::vector<VoiceInfo> summaries;
                for (const VoiceInformation& v : SpeechSynthesizer::AllVoices())
                {
                    Voice voice;
                    voice.info = v;
                    voice.summary.name     = narrow(std::wstring(v.DisplayName().c_str()));
                    voice.summary.language = narrow(std::wstring(v.Language().c_str()));
                    voice.summary.gender   = (v.Gender() == ss::VoiceGender::Male) ? plus::tts::VoiceGender::Male : plus::tts::VoiceGender::Female;
                    voice.japanese = voice.summary.language.compare(0, 2, "ja") == 0;
                    summaries.push_back(voice.summary);
                    all.push_back(std::move(voice));
                }
                mVoices = std::move(all);
                std::lock_guard<std::mutex> lock(mMutex);
                mVoiceList = std::move(summaries);
            }

            // The voice for a run: the wanted one by name if it speaks the language, else one of the
            // gender, else any of the language; with no voice of the language, one of the gender of any
            // language, else any voice. adjust is the pitch factor to use when the voice found is not
            // of the gender wanted. nullptr only when the system lists no voice at all.
            const Voice* chooseVoice(bool japanese, plus::tts::VoiceGender gender, const std::string& preferred, float& adjust) const
            {
                adjust = 1.0f;
                const plus::tts::VoiceGender wanted = (gender == plus::tts::VoiceGender::Neutral) ? plus::tts::VoiceGender::Female : gender;
                const Voice* any_of_language = nullptr;
                const Voice* of_gender       = nullptr;
                const Voice* any_voice       = nullptr;
                const Voice* any_of_gender   = nullptr;
                for (const Voice& v : mVoices)
                {
                    if (!any_voice)
                    {
                        any_voice = &v;
                    }
                    if (!any_of_gender && v.summary.gender == wanted)
                    {
                        any_of_gender = &v;
                    }
                    if (v.japanese != japanese)
                    {
                        continue;
                    }
                    if (!preferred.empty() && v.summary.name == preferred)
                    {
                        return &v;
                    }
                    if (!any_of_language)
                    {
                        any_of_language = &v;
                    }
                    if (!of_gender && v.summary.gender == wanted)
                    {
                        of_gender = &v;
                    }
                }
                if (of_gender)
                {
                    return of_gender;
                }
                // Only the other gender speaks it: lower or raise the voice to come near.
                const Voice* found = any_of_language ? any_of_language : (any_of_gender ? any_of_gender : any_voice);
                if (found && found->summary.gender != wanted)
                {
                    adjust = pitchToward(wanted);
                }
                return found;
            }

            static float pitchToward(plus::tts::VoiceGender wanted)
            {
                return (wanted == plus::tts::VoiceGender::Male) ? 0.8f : 1.2f;
            }

            void speakNow(const Utterance& utterance, unsigned generation)
            {
                for (const Run& run : splitByLanguage(widen(utterance.text)))
                {
                    if (!std::any_of(run.text.begin(), run.text.end(), isLetter) && run.text.find_first_of(L"0123456789") == std::wstring::npos)
                    {
                        continue;   // nothing to read in a run of signs
                    }
                    if (cancelled(generation))
                    {
                        return;
                    }
                    const std::string& preferred = (utterance.gender == plus::tts::VoiceGender::Male) ? utterance.preferredMale
                                                                                                      : utterance.preferredFemale;
                    float adjust = 1.0f;
                    const Voice* voice = chooseVoice(run.japanese, utterance.gender, preferred, adjust);
                    if (voice)
                    {
                        mSynth.Voice(voice->info);
                    }
                    else
                    {
                        // No voice listed: say it with the system's, not with the one of the run before.
                        const VoiceInformation fallback = SpeechSynthesizer::DefaultVoice();
                        mSynth.Voice(fallback);
                        const plus::tts::VoiceGender wanted = (utterance.gender == plus::tts::VoiceGender::Neutral) ? plus::tts::VoiceGender::Female : utterance.gender;
                        const plus::tts::VoiceGender actual = (fallback.Gender() == ss::VoiceGender::Male) ? plus::tts::VoiceGender::Male : plus::tts::VoiceGender::Female;
                        if (actual != wanted)
                        {
                            adjust = pitchToward(wanted);
                        }
                    }
                    auto options = mSynth.Options();
                    options.AudioPitch(std::clamp(static_cast<double>(utterance.pitch * adjust), 0.3, 1.9));
                    options.SpeakingRate(std::clamp(static_cast<double>(utterance.rate), 0.5, 4.0));
                    options.AudioVolume(1.0);   // the volume is applied to the samples (a quiet voice stays clear)

                    auto stream = mSynth.SynthesizeTextToStreamAsync(winrt::hstring(run.text)).get();
                    const uint64_t size = stream.Size();
                    if (size == 0 || size > 64u * 1024u * 1024u || cancelled(generation))
                    {
                        continue;
                    }
                    winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                    reader.LoadAsync(static_cast<uint32_t>(size)).get();
                    std::vector<uint8_t> wav(static_cast<size_t>(size));
                    reader.ReadBytes(wav);
                    applyVolume(wav, mVolume.load());

                    if (cancelled(generation))
                    {
                        return;
                    }
                    if (!playAndWait(std::move(wav), generation))
                    {
                        return;     // stopped (clear(), dropSpeaker(), shutdown())
                    }
                }
            }

            // What the device reads while it plays: the WAV and its header. On the heap, so that a device
            // that will not give the buffer back keeps them (let leak) instead of reading freed memory.
            struct Playback
            {
                std::vector<uint8_t> wav;
                WAVEHDR              header{};
            };

            enum class WaitEnd { Done, Stopped, TimedOut };

            // Waits until the device marks the header done, the line is stopped, or the deadline passes.
            // dwFlags is written by the driver: read it as volatile.
            WaitEnd waitDone(const WAVEHDR& header, unsigned generation, std::chrono::steady_clock::time_point deadline) const
            {
                while (!(static_cast<const volatile DWORD&>(header.dwFlags) & WHDR_DONE))
                {
                    if (cancelled(generation))
                    {
                        return WaitEnd::Stopped;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return WaitEnd::TimedOut;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                return WaitEnd::Done;
            }

            // Plays a PCM WAV in memory with waveOut and waits for its real end (the device marks the
            // buffer done), however late the device starts; not for a guess of its length, which let the
            // next line cut the end of this one. A clear() or shutdown() is seen within 20 ms and stops the
            // sound here, on this thread. A device that never gives the buffer back (an output unplugged)
            // is given up after the length of the sound and 2 s more. False only when stopped; a WAV that
            // cannot be played, or that the device did not finish, is skipped.
            bool playAndWait(std::vector<uint8_t>&& wav, unsigned generation)
            {
                auto play = std::make_unique<Playback>();
                play->wav = std::move(wav);     // the samples stay where they are: parts below point into them
                WavParts parts;
                if (!findWavParts(play->wav, parts))
                {
                    logLine("WinRT speech: a line's sound is not a WAV that can be played");
                    return true;
                }
                // The fmt chunk is a WAVEFORMATEX (or the 16-byte PCMWAVEFORMAT, which has no cbSize).
                std::vector<uint8_t> format_bytes(std::max<size_t>(parts.formatSize, sizeof(WAVEFORMATEX)), 0);
                std::memcpy(format_bytes.data(), parts.format, parts.formatSize);
                WAVEFORMATEX* format = reinterpret_cast<WAVEFORMATEX*>(format_bytes.data());
                if (parts.formatSize < sizeof(WAVEFORMATEX))
                {
                    format->cbSize = 0;
                }
                else
                {
                    // never more extra bytes than the chunk holds (the device reads cbSize of them)
                    format->cbSize = static_cast<WORD>(std::min<size_t>(format->cbSize, parts.formatSize - sizeof(WAVEFORMATEX)));
                }
                size_t length = parts.dataSize;
                if (format->nBlockAlign > 1)
                {
                    length -= length % format->nBlockAlign;
                }
                if (length == 0)
                {
                    return true;
                }
                // The longest the sound can take, and room for a device that starts late.
                uint64_t bytes_per_second = format->nAvgBytesPerSec;
                if (bytes_per_second == 0)
                {
                    bytes_per_second = static_cast<uint64_t>(format->nSamplesPerSec) * format->nBlockAlign;
                }
                const std::chrono::milliseconds limit = (bytes_per_second > 0)
                    ? std::chrono::milliseconds(static_cast<uint64_t>(length) * 1000u / bytes_per_second) + std::chrono::seconds(2)
                    : std::chrono::milliseconds(std::chrono::seconds(60));

                HWAVEOUT device = nullptr;
                if (waveOutOpen(&device, WAVE_MAPPER, format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
                {
                    logLine("WinRT speech: the sound device could not be opened");
                    return true;
                }
                WAVEHDR& header = play->header;
                header.lpData         = reinterpret_cast<LPSTR>(const_cast<uint8_t*>(parts.data));
                header.dwBufferLength = static_cast<DWORD>(length);
                bool stopped = false;
                bool keep    = false;   // the device may still hold the buffer: let it leak rather than free it
                if (waveOutPrepareHeader(device, &header, sizeof(header)) == MMSYSERR_NOERROR)
                {
                    if (waveOutWrite(device, &header, sizeof(header)) == MMSYSERR_NOERROR)
                    {
                        switch (waitDone(header, generation, std::chrono::steady_clock::now() + limit))
                        {
                        case WaitEnd::Done:
                            // Finished by itself: no reset, which could drop the last of the sound still in the device.
                            break;
                        case WaitEnd::Stopped:
                            stopped = true;
                            waveOutReset(device);   // gives the buffer back, marked done
                            break;
                        case WaitEnd::TimedOut:
                            logLine("WinRT speech: the sound device did not finish a line in time; it is stopped");
                            waveOutReset(device);
                            break;
                        }
                    }
                    else
                    {
                        logLine("WinRT speech: the sound device did not take a line's sound");
                    }
                    MMRESULT unprepared = waveOutUnprepareHeader(device, &header, sizeof(header));
                    if (unprepared == WAVERR_STILLPLAYING)
                    {
                        waveOutReset(device);
                        // a short wait for the buffer to come back, whether or not the line is stopped meanwhile
                        const auto retry_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                        while (!(static_cast<const volatile DWORD&>(header.dwFlags) & WHDR_DONE)
                               && std::chrono::steady_clock::now() < retry_end)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        unprepared = waveOutUnprepareHeader(device, &header, sizeof(header));
                    }
                    if (unprepared != MMSYSERR_NOERROR)
                    {
                        logLine("WinRT speech: the sound device kept a line's buffer; it is left to it");
                        keep = true;
                    }
                }
                else
                {
                    logLine("WinRT speech: a line's sound could not be prepared for the device");
                }
                if (waveOutClose(device) != MMSYSERR_NOERROR)
                {
                    waveOutReset(device);
                    if (waveOutClose(device) != MMSYSERR_NOERROR)
                    {
                        logLine("WinRT speech: the sound device could not be closed; it is left open");
                        keep = true;
                    }
                }
                if (keep)
                {
                    (void)play.release();   // a small leak, never a read of freed memory
                }
                return !stopped;
            }

            bool cancelled(unsigned generation) const
            {
                std::lock_guard<std::mutex> lock(mMutex);
                return mQuit || generation != mGeneration;
            }

            mutable std::mutex      mMutex;
            std::condition_variable mWake;
            std::deque<Utterance>   mQueue;
            std::vector<VoiceInfo>  mVoiceList;     // for voices(); under mMutex
            unsigned                mGeneration = 0;    // counts up at each clear(); under mMutex
            bool                    mQuit = false;      // under mMutex
            std::atomic<bool>       mReady{ false };
            std::atomic<float>      mVolume{ 1.0f };    // setVolume(); read by the worker for each piece

            std::atomic<bool>       mFailed{ false };   // gave up starting
            std::string             mSpeaking;          // the speaker of the line spoken now; under mMutex

            // Worker thread only.
            SpeechSynthesizer       mSynth{ nullptr };
            std::vector<Voice>      mVoices;

            std::thread             mThread;    // last: started after the rest is built
        };
    }

    std::unique_ptr<Engine> createWinRTEngine()
    {
        return std::make_unique<WinRTEngine>();
    }
}
