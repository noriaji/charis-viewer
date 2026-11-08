/**
 * @file subtitleclient.cpp
 * @brief Subtitles (plus/subtitle, research/137): the voice tap and the recognition helper.
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

#include "llviewerprecompiledheaders.h"

#include "subtitleclient.h"
#include "helperprocess.h"
#include "streamtap.h"
#include "subtitlering.h"

#include "lldir.h"
#include "llfile.h"
#include "llstring.h"
#include "llwebrtc.h"

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#if LL_WINDOWS
#include <windows.h>
#endif

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            // ---- the tap -------------------------------------------------------------------------

            constexpr size_t RING_SECONDS = 20;

            std::atomic<uint64_t> sProduced{ 0 };

            Ring& ring()
            {
                // Never destroyed (made once, on purpose not freed): the audio thread may still call onPlayout()
                // while statics are destroyed at exit. A function-local static object would be destroyed then.
                static Ring* const sRing = new Ring(size_t(RATE) * RING_SECONDS);
                return *sRing;
            }

            // Runs on the audio device's thread: converts and copies, nothing else.
            void onPlayout(const int16_t* samples, size_t frames, int channels, int sample_rate, void*)
            {
                static Converter sConverter;
                int16_t out[1024];
                const size_t n = sConverter.convert(samples, frames, channels, sample_rate, out, sizeof(out) / sizeof(out[0]));
                sProduced.fetch_add(ring().write(out, n), std::memory_order_relaxed);
            }

            std::atomic<size_t> sRefusedAtStart{ 0 };

            // ---- frames to the helper ---------------------------------------------------------------

            enum FrameType : uint8_t { FRAME_PCM = 1, FRAME_FLUSH = 2, FRAME_QUIT = 3, FRAME_VOICED = 4 };
            // FRAME_VOICED's payload: nobody speaking, somebody, not known (the helper cuts by loudness)
            enum Voiced : uint8_t { VOICED_NO = 0, VOICED_YES = 1, VOICED_UNKNOWN = 2 };

#if LL_WINDOWS
            // The longest line from the helper that is kept while its end has not come (a sentence is far shorter).
            constexpr size_t MAX_PENDING = 1 << 20;

            // How long stop() lets the helper obey QUIT before it is ended (on the main thread: keep it short).
            constexpr DWORD QUIT_GRACE_MS = 100;

            // After the voice was heard, the radio waits this long before it is taken again (research/192).
            constexpr std::chrono::milliseconds VOICE_HOLD{ 1500 };
            // A voice chunk whose loudest sample is below this (about -44 dBFS) counts as silence.
            constexpr int16_t VOICE_AUDIBLE = 200;

            bool audible(const int16_t* samples, size_t n)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    if (samples[i] >= VOICE_AUDIBLE || samples[i] <= -VOICE_AUDIBLE)
                    {
                        return true;
                    }
                }
                return false;
            }

            bool writeFrame(HANDLE h, FrameType type, const void* payload, uint32_t len)
            {
                uint8_t head[5];
                head[0] = type;
                memcpy(head + 1, &len, 4);   // little endian on every platform this runs on
                return writeAll(h, head, 5) && (len == 0 || writeAll(h, payload, len));
            }
#endif
        }

        namespace tap
        {
            void start()
            {
                // a new session starts from now: what the ring still holds is old
                int16_t drop[2048];
                while (ring().read(drop, sizeof(drop) / sizeof(drop[0])) > 0)
                {
                }
                sRefusedAtStart = ring().refused();
                sProduced = 0;
                llwebrtc::setPlayoutTap(&onPlayout, nullptr);
            }

            void stop()
            {
                llwebrtc::setPlayoutTap(nullptr, nullptr);
            }

            size_t read(int16_t* dst, size_t max)
            {
                return ring().read(dst, max);
            }

            size_t refused()
            {
                return ring().refused() - sRefusedAtStart;
            }

            uint64_t producedSamples()
            {
                return sProduced.load(std::memory_order_relaxed);
            }
        }

        std::string modelFolder()
        {
            return gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "subtitle";
        }

        std::string helperPath(const char* name)
        {
            // Only next to the viewer (how it is shipped; the folder the installer or the user put the viewer in).
            // The model's folder (<AppData>/subtitle) is no longer looked in (security audit A L1): any program
            // the user runs can write there, the helpers are not signed and their builds have no fixed hash,
            // so a copy there could not be told from one put there by something else. A copy left there is
            // named in the log once a session, so that "missing" can be told from "in the old place".
            const std::string beside = gDirUtilp->getExecutableDir() + gDirUtilp->getDirDelimiter() + name;
            if (!LLFile::isfile(beside))
            {
                const std::string old_place = modelFolder() + gDirUtilp->getDirDelimiter() + name;
                static std::set<std::string> told; // main thread only (the subtitle window)
                if (LLFile::isfile(old_place) && told.insert(name).second)
                {
                    LL_WARNS("Subtitle") << "not starting " << old_place << ": a helper is started only from the viewer's folder ("
                                         << gDirUtilp->getExecutableDir() << "); move it there" << LL_ENDL;
                }
            }
            return beside;
        }

#if LL_WINDOWS

        struct Client::Impl
        {
            HelperProcess helper;
            std::thread writer;
            std::thread reader;
            std::atomic<bool> stopWriter{ false };
            std::atomic<bool> alive{ false };
            std::atomic<bool> ready{ false };     // set by the reader when the helper says "ready"
            std::atomic<bool> voiced{ false };    // setVoiced()
            std::atomic<bool> streamEnabled{ false };   // setStreamEnabled()
            std::atomic<uint64_t> sent{ 0 };      // samples handed to the helper (both sources)
            bool useFlags = false;

            // Which source each stretch of the helper's clock came from (writer adds, main thread asks).
            struct Segment
            {
                uint64_t from;
                uint64_t to;
                Event::Source source;
            };
            std::mutex segmentMutex;
            std::deque<Segment> segments;

            void record(Event::Source source, uint64_t samples)
            {
                const uint64_t from = sent.load(std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(segmentMutex);
                    if (!segments.empty() && segments.back().source == source && segments.back().to == from)
                    {
                        segments.back().to = from + samples;
                    }
                    else
                    {
                        segments.push_back({ from, from + samples, source });
                    }
                    // two minutes are plenty: a sentence is seconds long and is asked about soon after it ends
                    while (segments.size() > 1 && segments.front().to + uint64_t(RATE) * 120 < from + samples)
                    {
                        segments.pop_front();
                    }
                }
                sent.store(from + samples, std::memory_order_relaxed);
            }

            // The source that most of t0..t1 (helper seconds) came from; the voice when it is not known.
            Event::Source sourceOf(double t0, double t1)
            {
                const double a = t0 * RATE, b = t1 * RATE;
                double voice = 0.0, stream = 0.0;
                std::lock_guard<std::mutex> lk(segmentMutex);
                for (const Segment& g : segments)
                {
                    const double overlap = std::min(double(g.to), b) - std::max(double(g.from), a);
                    if (overlap > 0.0)
                    {
                        (g.source == Event::Source::Stream ? stream : voice) += overlap;
                    }
                }
                return stream > voice ? Event::Source::Stream : Event::Source::Voice;
            }

            std::mutex mutex;
            std::deque<Event> events;

            void push(Event&& e)
            {
                std::lock_guard<std::mutex> lk(mutex);
                events.push_back(std::move(e));
            }

            void writerLoop()
            {
                // Listen only once the helper can take the sound: what is heard while the model loads
                // would be old by the time it is written out. This thread is the ring's one reader.
                while (!ready && !stopWriter)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (stopWriter)
                {
                    writeFrame(helper.toHelper, FRAME_QUIT, nullptr, 0);
                    return;
                }
                sent = 0;
                {
                    std::lock_guard<std::mutex> lk(segmentMutex);
                    segments.clear();
                }
                tap::start();
                std::vector<int16_t> buf(RATE);   // up to a second per frame
                using Clock = std::chrono::steady_clock;
                Clock::time_point lastVoice = Clock::now() - VOICE_HOLD;   // the voice has not been heard yet
                uint64_t streamGeneration = streamtap::generation();
                Event::Source current = Event::Source::Voice;
                bool haveCurrent = false;
                for (;;)
                {
                    if (stopWriter)
                    {
                        // This thread is the pipe's one writer, so QUIT is sent from here: it never lands
                        // in the middle of a frame. stop() does not wait for the helper to obey it.
                        writeFrame(helper.toHelper, FRAME_QUIT, nullptr, 0);
                        return;
                    }
                    Event::Source source = Event::Source::Voice;
                    size_t n = tap::read(buf.data(), buf.size());
                    const bool radio = streamEnabled;
                    if (n > 0 && radio && !audible(buf.data(), n))
                    {
                        n = 0;   // the voice playout runs on in silence: that is no voice, the radio may be taken
                    }
                    if (n > 0)
                    {
                        lastVoice = Clock::now();
                    }
                    else if (radio)
                    {
                        // A capture that was started again leaves old radio in the ring: not wanted.
                        const uint64_t generation = streamtap::generation();
                        if (generation != streamGeneration)
                        {
                            streamGeneration = generation;
                            streamtap::discard();
                        }
                        if (Clock::now() - lastVoice < VOICE_HOLD)
                        {
                            streamtap::discard();   // the voice comes first; the radio of this moment is let go
                        }
                        else
                        {
                            n = streamtap::read(buf.data(), buf.size());
                            source = Event::Source::Stream;
                        }
                    }
                    if (n == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                        continue;
                    }
                    if (haveCurrent && source != current)
                    {
                        // two sources must not run into one sentence
                        if (!writeFrame(helper.toHelper, FRAME_FLUSH, nullptr, 0))
                        {
                            return;
                        }
                    }
                    current = source;
                    haveCurrent = true;
                    if (useFlags)
                    {
                        // The voice server's flags say nothing about the radio: its sentences are cut by
                        // loudness (a 0 there kept the helper from ever hearing it speak; fix-r5).
                        const uint8_t v = source == Event::Source::Stream ? VOICED_UNKNOWN : voiced ? VOICED_YES : VOICED_NO;
                        if (!writeFrame(helper.toHelper, FRAME_VOICED, &v, 1))
                        {
                            return;
                        }
                    }
                    if (!writeFrame(helper.toHelper, FRAME_PCM, buf.data(), uint32_t(n * sizeof(int16_t))))
                    {
                        return;   // the helper is gone; the reader reports it
                    }
                    record(source, n);
                }
            }

            static void parseLine(const std::string& line, Event& e, bool& ok)
            {
                ok = false;
                boost::system::error_code ec;
                boost::json::value v = boost::json::parse(line, ec);
                if (ec || !v.is_object())
                {
                    return;
                }
                const boost::json::object& o = v.as_object();
                auto str = [&](const char* key) {
                    const boost::json::value* p = o.if_contains(key);
                    return (p && p->is_string()) ? std::string(p->as_string().c_str()) : std::string();
                };
                auto num = [&](const char* key) {
                    const boost::json::value* p = o.if_contains(key);
                    return (p && p->is_number()) ? p->to_number<double>() : 0.0;
                };
                const std::string kind = str("event");
                if (kind == "loading")
                {
                    e.type = Event::Type::Loading;
                }
                else if (kind == "ready")
                {
                    e.type = Event::Type::Ready;
                }
                else if (kind == "text")
                {
                    e.type = Event::Type::Text;
                    e.t0 = num("t0");
                    e.t1 = num("t1");
                    e.lang = str("lang");
                    e.text = str("text");
                    e.computeMs = num("compute_ms");
                }
                else if (kind == "dropped")
                {
                    e.type = Event::Type::Dropped;
                    e.seconds = num("seconds");
                }
                else if (kind == "error")
                {
                    e.type = Event::Type::Error;
                    e.message = str("message");
                }
                else
                {
                    return;
                }
                ok = true;
            }

            void readerLoop()
            {
                // Until the helper closes its output (it exited). A line that is no event we know fails
                // parseLine() and is left out.
                readLines(helper.fromHelper, MAX_PENDING, [this](const std::string& line) {
                    Event e;
                    bool ok = false;
                    parseLine(line, e, ok);
                    if (ok)
                    {
                        if (e.type == Event::Type::Ready)
                        {
                            ready = true;
                        }
                        push(std::move(e));
                    }
                });
                alive = false;
                Event e;
                e.type = Event::Type::Exited;
                push(std::move(e));
            }

            // Joins a pipe thread without hanging. Once the helper is gone its pipes are broken and the
            // thread ends by itself; while it is still going (slow to die, or the pipe held elsewhere) a
            // blocking ReadFile / WriteFile is cancelled, and the thread ends on the failed call. The
            // first 50 ms are only waited: the usual end needs no cancel, and a thread still starting or
            // ending (other programs' DLLs may do their own I/O then) is left alone.
            static void finish(std::thread& t)
            {
                if (!t.joinable())
                {
                    return;
                }
                const HANDLE h = static_cast<HANDLE>(t.native_handle());
                for (int round = 0; WaitForSingleObject(h, 10) == WAIT_TIMEOUT; ++round)
                {
                    if (round >= 5)
                    {
                        CancelSynchronousIo(h);
                    }
                }
                t.join();
            }
        };

        Client::Client() : mImpl(new Impl) {}

        Client::~Client()
        {
            stop();
        }

        bool Client::running() const
        {
            return mImpl->alive;
        }

        bool Client::start(const std::string& exe_path, const std::string& model_path, int threads, const std::string& lang,
                           bool use_flags, std::string& error)
        {
            stop();
            Impl& m = *mImpl;

            // The language goes on the command line unquoted: only "auto" or a code of 2-3 small
            // letters (en, ja, yue) is let through, anything else is "auto".
            const bool lang_ok = lang == "auto" ||
                                 ((lang.size() == 2 || lang.size() == 3) &&
                                  std::all_of(lang.begin(), lang.end(), [](char c) { return c >= 'a' && c <= 'z'; }));
            const std::wstring args = L"-m \"" + ll_convert_string_to_wide(model_path) + L"\" -t " + std::to_wstring(threads < 1 ? 1 : threads) +
                                      L" -l " + ll_convert_string_to_wide(lang_ok ? lang : std::string("auto"));
            // exe_path is exactly the file that helperPath() checked
            if (!m.helper.start(exe_path, args, 1 << 20, 1 << 16, error))
            {
                return false;
            }

            m.stopWriter = false;
            m.ready = false;
            m.voiced = false;
            m.useFlags = use_flags;
            m.alive = true;
            {
                std::lock_guard<std::mutex> lk(m.mutex);
                m.events.clear();
            }
            m.reader = std::thread([&m] { m.readerLoop(); });
            m.writer = std::thread([&m] { m.writerLoop(); });
            return true;
        }

        void Client::stop()
        {
            Impl& m = *mImpl;
            if (!m.helper.process && !m.reader.joinable() && !m.writer.joinable())
            {
                return;
            }
            // The writer sends QUIT on its way out. An idle helper obeys within a few tens of ms; one in the
            // middle of a sentence only reads it when the sentence is done (a second or more), and the main
            // thread is not held for that: the helper keeps nothing, so ending it loses nothing.
            m.stopWriter = true;
            m.helper.endWithin(QUIT_GRACE_MS);
            Impl::finish(m.writer);
            Impl::finish(m.reader);
            // after the writer: it is the one that starts the tap, so it cannot start it again behind us
            tap::stop();
            m.helper.close();
            m.alive = false;
            std::lock_guard<std::mutex> lk(m.mutex);
            m.events.clear();
        }

        void Client::setVoiced(bool voiced)
        {
            mImpl->voiced = voiced;
        }

        void Client::setStreamEnabled(bool enabled)
        {
            mImpl->streamEnabled = enabled;
        }

        double Client::sentSeconds() const
        {
            return double(mImpl->sent.load(std::memory_order_relaxed)) / RATE;
        }

        bool Client::poll(Event& out)
        {
            std::lock_guard<std::mutex> lk(mImpl->mutex);
            if (mImpl->events.empty())
            {
                return false;
            }
            out = std::move(mImpl->events.front());
            mImpl->events.pop_front();
            if (out.type == Event::Type::Text)
            {
                out.source = mImpl->sourceOf(out.t0, out.t1);
            }
            return true;
        }

#else   // not Windows: the helper is built for Windows only (research/137)

        struct Client::Impl {};
        Client::Client() : mImpl(new Impl) {}
        Client::~Client() {}
        bool Client::running() const { return false; }
        bool Client::start(const std::string&, const std::string&, int, const std::string&, bool, std::string& error)
        {
            error = "subtitles are available on Windows only";
            return false;
        }
        void Client::stop() {}
        bool Client::poll(Event&) { return false; }
        void Client::setVoiced(bool) {}
        void Client::setStreamEnabled(bool) {}
        double Client::sentSeconds() const { return 0.0; }

#endif
    }
}
