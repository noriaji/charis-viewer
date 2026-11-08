/**
 * @file translateclient.cpp
 * @brief Subtitles (plus/subtitle, research/217): the local translation helper, charis-translate.exe.
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

#include "translateclient.h"
#include "helperprocess.h"

#include "llstring.h"

#include <boost/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if LL_WINDOWS
#include <windows.h>
#endif

namespace plus
{
    namespace subtitle
    {
#if LL_WINDOWS

        namespace
        {
            // How long stop() lets the helper obey "quit" before it is ended (on the main thread: keep it short).
            constexpr DWORD QUIT_GRACE_MS = 100;
            // The longest line from the helper that is kept while its end has not come.
            constexpr size_t MAX_PENDING = 1 << 20;
            // Requests waiting for the writer: more than this means the helper is stuck; the newest are refused.
            constexpr size_t MAX_QUEUED = 64;
            // What the writer sends last (the helper's own command to end).
            constexpr char QUIT_LINE[] = "{\"cmd\":\"quit\"}\n";

            std::string quote(const std::string& s)
            {
                return boost::json::serialize(boost::json::value(s));
            }
        }

        struct TranslateClient::Impl
        {
            HelperProcess helper;
            std::thread writer;
            std::thread reader;
            std::atomic<bool> stopping{ false };
            std::atomic<bool> alive{ false };

            std::mutex outMutex;
            std::condition_variable outCv;
            std::deque<std::string> outgoing;

            std::mutex inMutex;
            std::deque<Result> results;

            void pushResult(Result&& r)
            {
                std::lock_guard<std::mutex> lk(inMutex);
                results.push_back(std::move(r));
            }

            void writerLoop()
            {
                for (;;)
                {
                    std::string line;
                    {
                        std::unique_lock<std::mutex> lk(outMutex);
                        outCv.wait(lk, [this] { return stopping || !outgoing.empty(); });
                        if (stopping)
                        {
                            break;
                        }
                        line = std::move(outgoing.front());
                        outgoing.pop_front();
                    }
                    if (!writeAll(helper.toHelper, line.data(), line.size()))
                    {
                        break;   // the helper is gone
                    }
                }
                // "quit" goes from here: this thread is the pipe's one writer, so it never lands inside a line
                writeAll(helper.toHelper, QUIT_LINE, sizeof(QUIT_LINE) - 1);
            }

            void parseLine(const std::string& line)
            {
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
                const boost::json::value* idv = o.if_contains("id");
                const std::string type = str("type");
                if (type == "text" && idv && idv->is_number())
                {
                    Result r;
                    r.id = idv->to_number<uint64_t>();
                    r.ok = true;
                    r.text = str("text");
                    pushResult(std::move(r));
                }
                else if (type == "error")
                {
                    Result r;
                    r.ok = false;
                    r.message = str("message");
                    // an error that belongs to a sentence carries its id; the others (a model that cannot be loaded
                    // comes with the sentence that needed it) are only for the log
                    r.id = (idv && idv->is_number() && idv->to_number<int64_t>() >= 0) ? idv->to_number<uint64_t>() : 0;
                    pushResult(std::move(r));
                }
            }

            void readerLoop()
            {
                // until the helper closes its output (it exited)
                readLines(helper.fromHelper, MAX_PENDING, [this](const std::string& line) { parseLine(line); });
                alive = false;
                outCv.notify_all();
            }
        };

        TranslateClient::TranslateClient() : mImpl(new Impl) {}

        TranslateClient::~TranslateClient()
        {
            stop();
        }

        bool TranslateClient::running() const
        {
            return mImpl->alive;
        }

        bool TranslateClient::start(const std::string& exe_path, const std::string& models, int threads, std::string& error)
        {
            stop();
            Impl& m = *mImpl;

            const std::wstring args = L"--models \"" + ll_convert_string_to_wide(models) + L"\" -t " + std::to_wstring(threads < 1 ? 1 : threads);
            // exe_path is exactly the file that helperPath() checked
            if (!m.helper.start(exe_path, args, 1 << 16, 1 << 16, error))
            {
                return false;
            }

            m.stopping = false;
            m.alive = true;
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                m.outgoing.clear();
            }
            {
                std::lock_guard<std::mutex> lk(m.inMutex);
                m.results.clear();
            }
            m.reader = std::thread([&m] { m.readerLoop(); });
            m.writer = std::thread([&m] { m.writerLoop(); });
            return true;
        }

        void TranslateClient::stop()
        {
            Impl& m = *mImpl;
            if (!m.helper.process && !m.reader.joinable() && !m.writer.joinable())
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                m.stopping = true;
            }
            m.outCv.notify_all();
            // The writer sends "quit" on its way out. An idle helper obeys at once; it is never waited for longer than
            // this, and one in the middle of a sentence is ended: it keeps nothing.
            m.helper.endWithin(QUIT_GRACE_MS);
            // The helper is gone now, so its pipe ends are broken and both threads leave their blocking calls.
            if (m.writer.joinable())
            {
                m.writer.join();
            }
            if (m.reader.joinable())
            {
                m.reader.join();
            }
            m.helper.close();
            m.alive = false;
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                m.outgoing.clear();
            }
            std::lock_guard<std::mutex> lk(m.inMutex);
            m.results.clear();
        }

        bool TranslateClient::request(uint64_t id, const std::string& from, const std::string& to, const std::string& text)
        {
            Impl& m = *mImpl;
            if (!m.alive)
            {
                return false;
            }
            const std::string line = "{\"id\":" + std::to_string(id) + ",\"from\":" + quote(from) + ",\"to\":" + quote(to) +
                                     ",\"text\":" + quote(text) + "}\n";
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                if (m.outgoing.size() >= MAX_QUEUED)
                {
                    return false;
                }
                m.outgoing.push_back(line);
            }
            m.outCv.notify_one();
            return true;
        }

        bool TranslateClient::poll(Result& out)
        {
            std::lock_guard<std::mutex> lk(mImpl->inMutex);
            if (mImpl->results.empty())
            {
                return false;
            }
            out = std::move(mImpl->results.front());
            mImpl->results.pop_front();
            return true;
        }

#else   // not Windows: the helper is built for Windows only

        struct TranslateClient::Impl {};
        TranslateClient::TranslateClient() : mImpl(new Impl) {}
        TranslateClient::~TranslateClient() {}
        bool TranslateClient::running() const { return false; }
        bool TranslateClient::start(const std::string&, const std::string&, int, std::string& error)
        {
            error = "local translation is available on Windows only";
            return false;
        }
        void TranslateClient::stop() {}
        bool TranslateClient::request(uint64_t, const std::string&, const std::string&, const std::string&) { return false; }
        bool TranslateClient::poll(Result&) { return false; }

#endif
    }
}
