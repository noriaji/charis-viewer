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
#include "lineclient.h"

#include "llstring.h"

#include <boost/json.hpp>

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            std::string quote(const std::string& s)
            {
                return boost::json::serialize(boost::json::value(s));
            }
        }

        struct TranslateClient::Impl
        {
            // Declared before the client, so that the client (and its reader thread, which pushes here) goes first.
            LineInbox<Result> results;
            LineClient lines;

            // On the reader thread (LineClient::LineHandler): the helper's answers become Results; the rest
            // ("ready", "loading", "loaded", lines we cannot read) are dropped.
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
                // the id as a whole number that fits (1.5, 1e30 or -1 do not: no id), without throwing
                uint64_t id = 0;
                const boost::json::value* idv = o.if_contains("id");
                bool has_id = false;
                if (idv && idv->is_number())
                {
                    boost::system::error_code id_ec;
                    id = idv->to_number<uint64_t>(id_ec);
                    has_id = !id_ec;
                }
                const std::string type = str("type");
                if (type == "text" && has_id)
                {
                    Result r;
                    r.id = id;
                    r.ok = true;
                    r.text = str("text");
                    results.push(std::move(r));
                }
                else if (type == "error")
                {
                    Result r;
                    r.ok = false;
                    r.message = str("message");
                    // an error that belongs to a sentence carries its id; the others (a model that cannot be loaded
                    // comes with the sentence that needed it) are only for the log
                    r.id = has_id ? id : 0;
                    results.push(std::move(r));
                }
            }
        };

        TranslateClient::TranslateClient() : mImpl(new Impl) {}

        TranslateClient::~TranslateClient()
        {
            stop();
        }

        bool TranslateClient::running() const
        {
            return mImpl->lines.running();
        }

        bool TranslateClient::start(const std::string& exe_path, const std::string& models, int threads, std::string& error)
        {
            stop();
#if LL_WINDOWS
            Impl& m = *mImpl;

            const std::wstring args = L"--models \"" + ll_convert_string_to_wide(models) + L"\" -t " + std::to_wstring(threads < 1 ? 1 : threads);
            // exe_path is exactly the file that helperPath() checked
            // answers are parsed on the reader thread; the end of the helper needs no mark (running() turns false)
            return m.lines.start(exe_path, args, { [&m](const std::string& line) { m.parseLine(line); }, nullptr }, error);
#else   // the helper is built for Windows only
            error = "local translation is available on Windows only";
            return false;
#endif
        }

        void TranslateClient::stop()
        {
            // the reader is joined first, so nothing is pushed after the answers are dropped
            mImpl->lines.stop();
            mImpl->results.clear();
        }

        bool TranslateClient::request(uint64_t id, const std::string& from, const std::string& to, const std::string& text)
        {
            if (!mImpl->lines.running())
            {
                return false;   // send() says so too; this only saves making the line
            }
            return mImpl->lines.send("{\"id\":" + std::to_string(id) + ",\"from\":" + quote(from) + ",\"to\":" + quote(to) +
                                     ",\"text\":" + quote(text) + "}");
        }

        bool TranslateClient::poll(Result& out)
        {
            return mImpl->results.pop(out);
        }
    }
}
