/**
 * @file lineclient.cpp
 * @brief Subtitles (plus/subtitle, research/324 §2.5): the viewer's side of a helper that talks in lines.
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

#include "lineclient.h"
#include "helperprocess.h"

#include "llerror.h"

#include <atomic>
#include <condition_variable>
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
            // Both pipes' buffer sizes. (How long stop() waits and the longest line are helperprocess.h's
            // HELPER_QUIT_GRACE_MS and HELPER_MAX_LINE, shared with the recognition helper's client.)
            constexpr DWORD PIPE_BYTES = 1 << 16;
            // Lines waiting for the writer: more than this means the helper is stuck; the newest are refused.
            constexpr size_t MAX_QUEUED = 64;
            // What the writer sends last (the helpers' own command to end).
            constexpr char QUIT_LINE[] = "{\"cmd\":\"quit\"}\n";

            // The client whose reader thread this is (nullptr on any other thread). Set by the reader itself, so it
            // is right from its first line on, also before start() has stored the std::thread.
            thread_local const void* tReaderOf = nullptr;
        }

        struct LineClient::Impl
        {
            HelperProcess helper;
            Handlers handlers;   // set by start() before the threads run, left alone while they run
            std::thread writer;
            std::thread reader;
            std::atomic<bool> stopping{ false };
            std::atomic<bool> alive{ false };

            std::mutex outMutex;
            std::condition_variable outCv;
            std::deque<std::string> outgoing;

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

            // On this client's reader thread, i.e. inside one of its handlers: start() and stop() must not run there
            // (stop() would join the thread it runs on).
            bool onReaderThread() const
            {
                return tReaderOf == this;
            }

            void readerLoop()
            {
                tReaderOf = this;
                // until the helper closes its output (it exited). A line the handler throws on is dropped: an
                // exception must not leave this thread (std::terminate).
                readLines(helper.fromHelper, HELPER_MAX_LINE, [this](const std::string& line) {
                    try
                    {
                        handlers.onLine(line);
                    }
                    catch (...)
                    {
                        LL_WARNS_ONCE("Subtitle") << "dropped a line from a helper that could not be taken" << LL_ENDL;
                    }
                });
                alive = false;
                outCv.notify_all();
                if (handlers.onEnd)
                {
                    try
                    {
                        handlers.onEnd();
                    }
                    catch (...)
                    {
                        LL_WARNS_ONCE("Subtitle") << "the end of a helper could not be taken" << LL_ENDL;
                    }
                }
                tReaderOf = nullptr;
            }
        };

        LineClient::LineClient() : mImpl(new Impl) {}

        LineClient::~LineClient()
        {
            if (mImpl->onReaderThread())
            {
                // The reader cannot be joined from itself, and the threads cannot outlive what they use: this ends
                // the viewer either way, but with the reason in the log.
                LL_ERRS("Subtitle") << "a helper client was destroyed from its own line handler" << LL_ENDL;
            }
            stop();
        }

        bool LineClient::running() const
        {
            return mImpl->alive;
        }

        bool LineClient::start(const std::string& exe_path, const std::wstring& args, Handlers handlers, std::string& error)
        {
            Impl& m = *mImpl;
            if (m.onReaderThread())
            {
                LL_WARNS("Subtitle") << "a helper client's start() was called from its own line handler: not done" << LL_ENDL;
                error = "start() called from the client's own handler";
                return false;
            }
            stop();

            if (!handlers.onLine)
            {
                error = "no handler for the helper's lines";
                return false;
            }
            if (!m.helper.start(exe_path, args, PIPE_BYTES, PIPE_BYTES, error))
            {
                return false;
            }

            m.handlers = std::move(handlers);
            m.stopping = false;
            m.alive = true;
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                m.outgoing.clear();
            }
            m.reader = std::thread([&m] { m.readerLoop(); });
            m.writer = std::thread([&m] { m.writerLoop(); });
            return true;
        }

        void LineClient::stop()
        {
            Impl& m = *mImpl;
            if (m.onReaderThread())
            {
                LL_WARNS("Subtitle") << "a helper client's stop() was called from its own line handler: not done" << LL_ENDL;
                return;
            }
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
            // this, and one in the middle of a request is ended: it keeps nothing.
            m.helper.endWithin(HELPER_QUIT_GRACE_MS);
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
            m.handlers = Handlers();   // the reader is gone: nothing calls them now
            std::lock_guard<std::mutex> lk(m.outMutex);
            m.outgoing.clear();
        }

        bool LineClient::send(std::string line)
        {
            Impl& m = *mImpl;
            // a line end inside ('\n', or '\r' that a reader may take as one) would cut the request in two
            if (!m.alive || line.find_first_of("\r\n") != std::string::npos)
            {
                return false;
            }
            line += '\n';
            {
                std::lock_guard<std::mutex> lk(m.outMutex);
                if (m.outgoing.size() >= MAX_QUEUED)
                {
                    return false;
                }
                m.outgoing.push_back(std::move(line));
            }
            m.outCv.notify_one();
            return true;
        }

#else   // not Windows: the helpers are built for Windows only

        struct LineClient::Impl {};
        LineClient::LineClient() : mImpl(new Impl) {}
        LineClient::~LineClient() {}
        bool LineClient::running() const { return false; }
        bool LineClient::start(const std::string&, const std::wstring&, Handlers, std::string& error)
        {
            error = "helper programs are available on Windows only";
            return false;
        }
        void LineClient::stop() {}
        bool LineClient::send(std::string) { return false; }

#endif
    }
}
