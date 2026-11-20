/**
 * @file lineclient.h
 * @brief Subtitles (plus/subtitle, research/324 §2.5): the viewer's side of a helper that talks in lines.
 *
 * A helper program (charis-translate.exe now) reads one JSON line per request on its stdin and answers one JSON line
 * each on its stdout. LineClient is what every such client shares: the helper on two pipes in a job
 * (HelperProcess), one writer thread (requests, then "quit" on the way out: the pipe's one writer, so a line is
 * never cut), one reader thread (lines from the helper, handed to the client's own handlers), and the order in which
 * it all stops. What the lines say is the client's own. Two ways to use it:
 *  - parse on the reader thread and push what came of it into a LineInbox<Result> (TranslateClient);
 *  - take the lines as they are into a HelperLineInbox (at most HELPER_LINE_INBOX_MAX, then an Overflow mark; an
 *    End mark when the helper's output closed) and parse them on the main thread (research/324 §2.5, §5.2).
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

#ifndef PLUS_SUBTITLE_LINECLIENT_H
#define PLUS_SUBTITLE_LINECLIENT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace plus
{
    namespace subtitle
    {
        class LineClient
        {
        public:
            // Called on the reader thread with each line from the helper (without its line end), never after stop()
            // has returned. Parsing here is fine; keep any lock short (move the result in). Never call start() or
            // stop() from it, nor destroy the client: stop() would join the thread it runs on, so both refuse there
            // (a warning in the log, start() returns false); destroying it there ends the viewer, with the reason in
            // the log (LL_ERRS). What it throws is caught and the line dropped (logged once).
            using LineHandler = std::function<void(const std::string& line)>;
            // Called on the reader thread once, after the last line, when the helper's output has closed: it ended by
            // itself, crashed, or stop() ended it. running() is already false then. The same rules as LineHandler.
            using EndHandler = std::function<void()>;

            struct Handlers
            {
                LineHandler onLine;   // required
                EndHandler onEnd;     // may be empty
            };

            LineClient();
            ~LineClient();   // stops
            LineClient(const LineClient&) = delete;
            LineClient& operator=(const LineClient&) = delete;

            // Stops what runs, then starts exe_path (exactly that file: the caller has checked it, helperPath()) with
            // args as they are (the caller quotes what needs it). What the handlers touch must outlive the threads
            // (declare it before the LineClient). False (and why) when it cannot, also when onLine is empty; nothing
            // is left running then. Called from this client's own handler it refuses instead: false and why, nothing
            // stopped, and the run that is going goes on. The stop() at its head also ends the last run's reader, which calls that run's
            // onEnd: to start again with an inbox, do stop(), then the inbox's clear(), then start(), or an old End
            // (and an old overflow) is left in it.
            bool start(const std::string& exe_path, const std::wstring& args, Handlers handlers, std::string& error);
            // Tells the helper to quit, gives it a moment (HELPER_QUIT_GRACE_MS, helperprocess.h) and then ends it,
            // joins both threads and drops what was not sent. Safe when not running. Blocks the caller for at most
            // that moment plus the joins.
            void stop();
            // The helper is there and has not closed its output.
            bool running() const;

            // Queues one line for the helper (the line end is added here). Any thread, but never at the same time as
            // start(), stop() or the destructor. False when the helper is not running, when the line holds a line end
            // of its own ('\n' or '\r': it would be read as two lines), or when too many lines are waiting (a stuck
            // helper: the newest are refused).
            bool send(std::string line);

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };

        // What the reader thread hands to the main thread: pushed under the lock (moved in), taken one at a time by
        // pop(). Without a limit by default. With one, the first push that finds it full puts in the overflow mark
        // instead, and every push after that is dropped until clear(): the main thread sees the items that came
        // before, then the mark, and nothing out of order.
        template <class T>
        class LineInbox
        {
        public:
            LineInbox() = default;
            LineInbox(size_t max_items, T overflow_mark) : mMax(max_items), mOverflowMark(std::move(overflow_mark)) {}
            LineInbox(const LineInbox&) = delete;
            LineInbox& operator=(const LineInbox&) = delete;

            // False when the item was not taken (full, or overflowed before).
            bool push(T&& item)
            {
                std::lock_guard<std::mutex> lk(mMutex);
                if (mOverflowed)
                {
                    return false;
                }
                if (mMax && mItems.size() >= mMax)
                {
                    mItems.push_back(mOverflowMark);
                    mOverflowed = true;
                    return false;
                }
                mItems.push_back(std::move(item));
                return true;
            }

            // Taken even when full or overflowed (the end mark, which the main thread must see).
            void pushLast(T&& item)
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mItems.push_back(std::move(item));
            }

            bool pop(T& out)
            {
                std::lock_guard<std::mutex> lk(mMutex);
                if (mItems.empty())
                {
                    return false;
                }
                out = std::move(mItems.front());
                mItems.pop_front();
                return true;
            }

            // Empties it and forgets an overflow. Call it after LineClient::stop(), when nothing pushes any more.
            void clear()
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mItems.clear();
                mOverflowed = false;
            }

        private:
            std::mutex mMutex;
            std::deque<T> mItems;
            size_t mMax = 0;   // 0: no limit
            T mOverflowMark{};
            bool mOverflowed = false;
        };

        // One line from a helper as it came, or a mark in its place.
        struct HelperLine
        {
            enum class Kind : uint8_t
            {
                Text,       // text is the line
                Overflow,   // more than HELPER_LINE_INBOX_MAX lines came before the main thread took them; the rest
                            // are dropped (a helper that says far too much is broken: stop it)
                End         // the helper's output closed (it is gone)
            };
            Kind kind = Kind::Text;
            std::string text;
        };

        // Lines that may wait for the main thread: a helper answers one line per request, plus a few about itself.
        constexpr size_t HELPER_LINE_INBOX_MAX = 8;

        // The lines as they came, for a client that parses them on the main thread (outside the lock). Declare it
        // before the LineClient that was given its handlers(), and clear() it after that client's stop(). To start
        // the helper again: stop(), clear(), start() (start()'s own stop() puts the last run's End in too).
        class HelperLineInbox : public LineInbox<HelperLine>
        {
        public:
            HelperLineInbox() : LineInbox<HelperLine>(HELPER_LINE_INBOX_MAX, HelperLine{ HelperLine::Kind::Overflow, {} }) {}

            // For LineClient::start: each line as Text, and End when the output closed.
            LineClient::Handlers handlers()
            {
                return { [this](const std::string& line) { push(HelperLine{ HelperLine::Kind::Text, line }); },
                         [this] { pushLast(HelperLine{ HelperLine::Kind::End, {} }); } };
            }
        };
    }
}

#endif // PLUS_SUBTITLE_LINECLIENT_H
